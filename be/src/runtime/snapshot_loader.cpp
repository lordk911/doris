// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "runtime/snapshot_loader.h"

// IWYU pragma: no_include <bthread/errno.h>
#include <errno.h> // IWYU pragma: keep
#include <fmt/format.h>
#include <gen_cpp/FrontendService.h>
#include <gen_cpp/FrontendService_types.h>
#include <gen_cpp/HeartbeatService_types.h>
#include <gen_cpp/PlanNodes_types.h>
#include <gen_cpp/Status_types.h>
#include <gen_cpp/Types_types.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <istream>
#include <unordered_map>
#include <utility>

#include "common/logging.h"
#include "gutil/strings/split.h"
#include "http/http_client.h"
#include "io/fs/broker_file_system.h"
#include "io/fs/file_system.h"
#include "io/fs/hdfs_file_system.h"
#include "io/fs/local_file_system.h"
#include "io/fs/path.h"
#include "io/fs/remote_file_system.h"
#include "io/fs/s3_file_system.h"
#include "io/hdfs_builder.h"
#include "olap/data_dir.h"
#include "olap/snapshot_manager.h"
#include "olap/storage_engine.h"
#include "olap/tablet.h"
#include "olap/tablet_manager.h"
#include "runtime/client_cache.h"
#include "runtime/exec_env.h"
#include "util/s3_uri.h"
#include "util/s3_util.h"
#include "util/thrift_rpc_helper.h"

namespace doris {
namespace {

Status upload_with_checksum(io::RemoteFileSystem& fs, std::string_view local_path,
                            std::string_view remote_path, std::string_view checksum) {
    auto full_remote_path = fmt::format("{}.{}", remote_path, checksum);
    switch (fs.type()) {
    case io::FileSystemType::HDFS:
    case io::FileSystemType::BROKER: {
        std::string temp = fmt::format("{}.part", remote_path);
        RETURN_IF_ERROR(fs.upload(local_path, temp));
        RETURN_IF_ERROR(fs.rename(temp, full_remote_path));
        break;
    }
    case io::FileSystemType::S3:
        RETURN_IF_ERROR(fs.upload(local_path, full_remote_path));
        break;
    default:
        throw Exception(Status::FatalError("unknown fs type: {}", static_cast<int>(fs.type())));
    }
    return Status::OK();
}

bool _end_with(std::string_view str, std::string_view match) {
    return str.size() >= match.size() &&
           str.compare(str.size() - match.size(), match.size(), match) == 0;
}

} // namespace

SnapshotLoader::SnapshotLoader(StorageEngine& engine, ExecEnv* env, int64_t job_id, int64_t task_id,
                               const TNetworkAddress& broker_addr,
                               const std::map<std::string, std::string>& prop)
        : _engine(engine),
          _env(env),
          _job_id(job_id),
          _task_id(task_id),
          _broker_addr(broker_addr),
          _prop(prop) {}

Status SnapshotLoader::init(TStorageBackendType::type type, const std::string& location) {
    if (TStorageBackendType::type::S3 == type) {
        S3Conf s3_conf;
        S3URI s3_uri(location);
        RETURN_IF_ERROR(s3_uri.parse());
        RETURN_IF_ERROR(S3ClientFactory::convert_properties_to_s3_conf(_prop, s3_uri, &s3_conf));
        _remote_fs =
                DORIS_TRY(io::S3FileSystem::create(std::move(s3_conf), io::FileSystem::TMP_FS_ID));
    } else if (TStorageBackendType::type::HDFS == type) {
        THdfsParams hdfs_params = parse_properties(_prop);
        _remote_fs = DORIS_TRY(io::HdfsFileSystem::create(hdfs_params, hdfs_params.fs_name,
                                                          io::FileSystem::TMP_FS_ID, nullptr));
    } else if (TStorageBackendType::type::BROKER == type) {
        std::shared_ptr<io::BrokerFileSystem> fs;
        _remote_fs = DORIS_TRY(
                io::BrokerFileSystem::create(_broker_addr, _prop, io::FileSystem::TMP_FS_ID));
    } else {
        return Status::InternalError("Unknown storage type: {}", type);
    }
    return Status::OK();
}

SnapshotLoader::~SnapshotLoader() = default;

Status SnapshotLoader::upload(const std::map<std::string, std::string>& src_to_dest_path,
                              std::map<int64_t, std::vector<std::string>>* tablet_files) {
    if (!_remote_fs) {
        return Status::InternalError("Storage backend not initialized.");
    }
    LOG(INFO) << "begin to upload snapshot files. num: " << src_to_dest_path.size()
              << ", broker addr: " << _broker_addr << ", job: " << _job_id << ", task" << _task_id;

    // check if job has already been cancelled
    int tmp_counter = 1;
    RETURN_IF_ERROR(_report_every(0, &tmp_counter, 0, 0, TTaskType::type::UPLOAD));

    Status status = Status::OK();
    // 1. validate local tablet snapshot paths
    RETURN_IF_ERROR(_check_local_snapshot_paths(src_to_dest_path, true));

    // 2. for each src path, upload it to remote storage
    // we report to frontend for every 10 files, and we will cancel the job if
    // the job has already been cancelled in frontend.
    int report_counter = 0;
    int total_num = src_to_dest_path.size();
    int finished_num = 0;
    for (const auto& iter : src_to_dest_path) {
        const std::string& src_path = iter.first;
        const std::string& dest_path = iter.second;

        // Take a lock to protect the local snapshot path.
        auto local_snapshot_guard = LocalSnapshotLock::instance().acquire(src_path);

        int64_t tablet_id = 0;
        int32_t schema_hash = 0;
        RETURN_IF_ERROR(
                _get_tablet_id_and_schema_hash_from_file_path(src_path, &tablet_id, &schema_hash));

        // 2.1 get existing files from remote path
        std::map<std::string, FileStat> remote_files;
        RETURN_IF_ERROR(_list_with_checksum(dest_path, &remote_files));

        for (auto& tmp : remote_files) {
            VLOG_CRITICAL << "get remote file: " << tmp.first << ", checksum: " << tmp.second.md5;
        }

        // 2.2 list local files
        std::vector<std::string> local_files;
        std::vector<std::string> local_files_with_checksum;
        RETURN_IF_ERROR(_get_existing_files_from_local(src_path, &local_files));

        // 2.3 iterate local files
        for (auto& local_file : local_files) {
            RETURN_IF_ERROR(_report_every(10, &report_counter, finished_num, total_num,
                                          TTaskType::type::UPLOAD));

            // calc md5sum of localfile
            std::string md5sum;
            RETURN_IF_ERROR(
                    io::global_local_filesystem()->md5sum(src_path + "/" + local_file, &md5sum));
            VLOG_CRITICAL << "get file checksum: " << local_file << ": " << md5sum;
            local_files_with_checksum.push_back(local_file + "." + md5sum);

            // check if this local file need upload
            bool need_upload = false;
            auto find = remote_files.find(local_file);
            if (find != remote_files.end()) {
                if (md5sum != find->second.md5) {
                    // remote storage file exist, but with different checksum
                    LOG(WARNING) << "remote file checksum is invalid. remote: " << find->first
                                 << ", local: " << md5sum;
                    // TODO(cmy): save these files and delete them later
                    need_upload = true;
                }
            } else {
                need_upload = true;
            }

            if (!need_upload) {
                VLOG_CRITICAL << "file exist in remote path, no need to upload: " << local_file;
                continue;
            }

            // upload
            std::string remote_path = dest_path + '/' + local_file;
            std::string local_path = src_path + '/' + local_file;
            RETURN_IF_ERROR(upload_with_checksum(*_remote_fs, local_path, remote_path, md5sum));
        } // end for each tablet's local files

        tablet_files->emplace(tablet_id, local_files_with_checksum);
        finished_num++;
        LOG(INFO) << "finished to write tablet to remote. local path: " << src_path
                  << ", remote path: " << dest_path;
    } // end for each tablet path

    LOG(INFO) << "finished to upload snapshots. job: " << _job_id << ", task id: " << _task_id;
    return status;
}

/*
 * Download snapshot files from remote.
 * After downloaded, the local dir should contains all files existing in remote,
 * may also contains several useless files.
 */
Status SnapshotLoader::download(const std::map<std::string, std::string>& src_to_dest_path,
                                std::vector<int64_t>* downloaded_tablet_ids) {
    if (!_remote_fs) {
        return Status::InternalError("Storage backend not initialized.");
    }
    LOG(INFO) << "begin to download snapshot files. num: " << src_to_dest_path.size()
              << ", broker addr: " << _broker_addr << ", job: " << _job_id
              << ", task id: " << _task_id;

    // check if job has already been cancelled
    int tmp_counter = 1;
    RETURN_IF_ERROR(_report_every(0, &tmp_counter, 0, 0, TTaskType::type::DOWNLOAD));

    Status status = Status::OK();
    // 1. validate local tablet snapshot paths
    RETURN_IF_ERROR(_check_local_snapshot_paths(src_to_dest_path, false));

    // 2. for each src path, download it to local storage
    int report_counter = 0;
    int total_num = src_to_dest_path.size();
    int finished_num = 0;
    for (const auto& iter : src_to_dest_path) {
        const std::string& remote_path = iter.first;
        const std::string& local_path = iter.second;

        // Take a lock to protect the local snapshot path.
        auto local_snapshot_guard = LocalSnapshotLock::instance().acquire(local_path);

        int64_t local_tablet_id = 0;
        int32_t schema_hash = 0;
        RETURN_IF_ERROR(_get_tablet_id_and_schema_hash_from_file_path(local_path, &local_tablet_id,
                                                                      &schema_hash));
        downloaded_tablet_ids->push_back(local_tablet_id);

        int64_t remote_tablet_id;
        RETURN_IF_ERROR(_get_tablet_id_from_remote_path(remote_path, &remote_tablet_id));
        VLOG_CRITICAL << "get local tablet id: " << local_tablet_id
                      << ", schema hash: " << schema_hash
                      << ", remote tablet id: " << remote_tablet_id;

        // 2.1. get local files
        std::vector<std::string> local_files;
        RETURN_IF_ERROR(_get_existing_files_from_local(local_path, &local_files));

        // 2.2. get remote files
        std::map<std::string, FileStat> remote_files;
        RETURN_IF_ERROR(_list_with_checksum(remote_path, &remote_files));
        if (remote_files.empty()) {
            std::stringstream ss;
            ss << "get nothing from remote path: " << remote_path;
            LOG(WARNING) << ss.str();
            return Status::InternalError(ss.str());
        }

        TabletSharedPtr tablet = _engine.tablet_manager()->get_tablet(local_tablet_id);
        if (tablet == nullptr) {
            std::stringstream ss;
            ss << "failed to get local tablet: " << local_tablet_id;
            LOG(WARNING) << ss.str();
            return Status::InternalError(ss.str());
        }
        DataDir* data_dir = tablet->data_dir();

        for (auto& iter : remote_files) {
            RETURN_IF_ERROR(_report_every(10, &report_counter, finished_num, total_num,
                                          TTaskType::type::DOWNLOAD));

            bool need_download = false;
            const std::string& remote_file = iter.first;
            const FileStat& file_stat = iter.second;
            auto find = std::find(local_files.begin(), local_files.end(), remote_file);
            if (find == local_files.end()) {
                // remote file does not exist in local, download it
                need_download = true;
            } else {
                if (_end_with(remote_file, ".hdr")) {
                    // this is a header file, download it.
                    need_download = true;
                } else {
                    // check checksum
                    std::string local_md5sum;
                    Status st = io::global_local_filesystem()->md5sum(
                            local_path + "/" + remote_file, &local_md5sum);
                    if (!st.ok()) {
                        LOG(WARNING) << "failed to get md5sum of local file: " << remote_file
                                     << ". msg: " << st << ". download it";
                        need_download = true;
                    } else {
                        VLOG_CRITICAL << "get local file checksum: " << remote_file << ": "
                                      << local_md5sum;
                        if (file_stat.md5 != local_md5sum) {
                            // file's checksum does not equal, download it.
                            need_download = true;
                        }
                    }
                }
            }

            if (!need_download) {
                LOG(INFO) << "remote file already exist in local, no need to download."
                          << ", file: " << remote_file;
                continue;
            }

            // begin to download
            std::string full_remote_file = remote_path + "/" + remote_file + "." + file_stat.md5;
            std::string local_file_name;
            // we need to replace the tablet_id in remote file name with local tablet id
            RETURN_IF_ERROR(_replace_tablet_id(remote_file, local_tablet_id, &local_file_name));
            std::string full_local_file = local_path + "/" + local_file_name;
            LOG(INFO) << "begin to download from " << full_remote_file << " to " << full_local_file;
            size_t file_len = file_stat.size;

            // check disk capacity
            if (data_dir->reach_capacity_limit(file_len)) {
                return Status::Error<ErrorCode::EXCEEDED_LIMIT>(
                        "reach the capacity limit of path {}, file_size={}", data_dir->path(),
                        file_len);
            }
            // remove file which will be downloaded now.
            // this file will be added to local_files if it be downloaded successfully.
            if (find != local_files.end()) {
                local_files.erase(find);
            }
            RETURN_IF_ERROR(_remote_fs->download(full_remote_file, full_local_file));

            // 3. check md5 of the downloaded file
            std::string downloaded_md5sum;
            RETURN_IF_ERROR(
                    io::global_local_filesystem()->md5sum(full_local_file, &downloaded_md5sum));
            VLOG_CRITICAL << "get downloaded file checksum: " << full_local_file << ": "
                          << downloaded_md5sum;
            if (downloaded_md5sum != file_stat.md5) {
                std::stringstream ss;
                ss << "invalid md5 of downloaded file: " << full_local_file
                   << ", expected: " << file_stat.md5 << ", get: " << downloaded_md5sum;
                LOG(WARNING) << ss.str();
                return Status::InternalError(ss.str());
            }

            // local_files always keep the updated local files
            local_files.push_back(local_file_name);
            LOG(INFO) << "finished to download file via broker. file: " << full_local_file
                      << ", length: " << file_len;
        } // end for all remote files

        // finally, delete local files which are not in remote
        for (const auto& local_file : local_files) {
            // replace the tablet id in local file name with the remote tablet id,
            // in order to compare the file name.
            std::string new_name;
            Status st = _replace_tablet_id(local_file, remote_tablet_id, &new_name);
            if (!st.ok()) {
                LOG(WARNING) << "failed to replace tablet id. unknown local file: " << st
                             << ". ignore it";
                continue;
            }
            VLOG_CRITICAL << "new file name after replace tablet id: " << new_name;
            const auto& find = remote_files.find(new_name);
            if (find != remote_files.end()) {
                continue;
            }

            // delete
            std::string full_local_file = local_path + "/" + local_file;
            VLOG_CRITICAL << "begin to delete local snapshot file: " << full_local_file
                          << ", it does not exist in remote";
            if (remove(full_local_file.c_str()) != 0) {
                LOG(WARNING) << "failed to delete unknown local file: " << full_local_file
                             << ", ignore it";
            }
        }

        finished_num++;
    } // end for src_to_dest_path

    LOG(INFO) << "finished to download snapshots. job: " << _job_id << ", task id: " << _task_id;
    return status;
}

Status SnapshotLoader::remote_http_download(
        const std::vector<TRemoteTabletSnapshot>& remote_tablet_snapshots,
        std::vector<int64_t>* downloaded_tablet_ids) {
    constexpr uint32_t kListRemoteFileTimeout = 15;
    constexpr uint32_t kDownloadFileMaxRetry = 3;
    constexpr uint32_t kGetLengthTimeout = 10;

    // check if job has already been cancelled
    int tmp_counter = 1;
    RETURN_IF_ERROR(_report_every(0, &tmp_counter, 0, 0, TTaskType::type::DOWNLOAD));
    Status status = Status::OK();

    int report_counter = 0;
    int finished_num = 0;
    int total_num = remote_tablet_snapshots.size();
    for (const auto& remote_tablet_snapshot : remote_tablet_snapshots) {
        const auto& local_path = remote_tablet_snapshot.local_snapshot_path;
        const auto& remote_path = remote_tablet_snapshot.remote_snapshot_path;
        LOG(INFO) << fmt::format(
                "download snapshots via http. job: {}, task id: {}, local dir: {}, remote dir: {}",
                _job_id, _task_id, local_path, remote_path);

        // Take a lock to protect the local snapshot path.
        auto local_snapshot_guard = LocalSnapshotLock::instance().acquire(local_path);

        // Step 1: Validate local tablet snapshot paths
        bool res = true;
        RETURN_IF_ERROR(io::global_local_filesystem()->is_directory(local_path, &res));
        if (!res) {
            std::stringstream ss;
            auto err_msg =
                    fmt::format("snapshot path is not directory or does not exist: {}", local_path);
            LOG(WARNING) << err_msg;
            return Status::RuntimeError(err_msg);
        }

        // Step 2: get all local files
        struct LocalFileStat {
            uint64_t size;
            std::string md5;
        };
        std::unordered_map<std::string, LocalFileStat> local_files;
        std::vector<std::string> existing_files;
        RETURN_IF_ERROR(_get_existing_files_from_local(local_path, &existing_files));
        for (auto& local_file : existing_files) {
            // add file size
            std::string local_file_path = local_path + "/" + local_file;
            std::error_code ec;
            uint64_t local_file_size = std::filesystem::file_size(local_file_path, ec);
            if (ec) {
                LOG(WARNING) << "download file error" << ec.message();
                return Status::IOError("can't retrive file_size of {}, due to {}", local_file_path,
                                       ec.message());
            }
            std::string md5;
            auto status = io::global_local_filesystem()->md5sum(local_file_path, &md5);
            if (!status.ok()) {
                LOG(WARNING) << "download file error, local file " << local_file_path
                             << " md5sum: " << status.to_string();
                return status;
            }
            local_files[local_file] = {local_file_size, md5};
        }
        existing_files.clear();

        // Step 3: Validate remote tablet snapshot paths && remote files map
        // key is remote snapshot paths, value is filelist
        // get all these use http download action
        // http://172.16.0.14:6781/api/_tablet/_download?token=e804dd27-86da-4072-af58-70724075d2a4&file=/home/ubuntu/doris_master/output/be/storage/snapshot/20230410102306.9.180//2774718/217609978/2774718.hdr
        struct RemoteFileStat {
            std::string url;
            std::string md5;
            uint64_t size;
        };
        std::unordered_map<std::string, RemoteFileStat> remote_files;
        const auto& token = remote_tablet_snapshot.remote_token;
        const auto& remote_be_addr = remote_tablet_snapshot.remote_be_addr;

        // HEAD http://172.16.0.14:6781/api/_tablet/_download?token=e804dd27-86da-4072-af58-70724075d2a4&file=/home/ubuntu/doris_master/output/be/storage/snapshot/20230410102306.9.180/
        std::string base_url = fmt::format("http://{}:{}/api/_tablet/_download?token={}",
                                           remote_be_addr.hostname, remote_be_addr.port, token);
        std::string remote_url_prefix = fmt::format("{}&file={}", base_url, remote_path);

        LOG(INFO) << "list remote files: " << remote_url_prefix << ", job: " << _job_id
                  << ", task id: " << _task_id << ", remote be: " << remote_be_addr;
        string file_list_str;
        auto list_files_cb = [&remote_url_prefix, &file_list_str](HttpClient* client) {
            RETURN_IF_ERROR(client->init(remote_url_prefix));
            client->set_timeout_ms(kListRemoteFileTimeout * 1000);
            return client->execute(&file_list_str);
        };
        RETURN_IF_ERROR(HttpClient::execute_with_retry(kDownloadFileMaxRetry, 1, list_files_cb));
        std::vector<string> filename_list =
                strings::Split(file_list_str, "\n", strings::SkipWhitespace());

        for (const auto& filename : filename_list) {
            std::string remote_file_url =
                    fmt::format("{}&file={}/{}&channel=ingest_binlog", base_url,
                                remote_tablet_snapshot.remote_snapshot_path, filename);

            // get file length
            uint64_t file_size = 0;
            std::string file_md5;
            auto get_file_stat_cb = [&remote_file_url, &file_size, &file_md5](HttpClient* client) {
                std::string url = fmt::format("{}&acquire_md5=true", remote_file_url);
                RETURN_IF_ERROR(client->init(url));
                client->set_timeout_ms(kGetLengthTimeout * 1000);
                RETURN_IF_ERROR(client->head());
                RETURN_IF_ERROR(client->get_content_length(&file_size));
                RETURN_IF_ERROR(client->get_content_md5(&file_md5));
                return Status::OK();
            };
            RETURN_IF_ERROR(
                    HttpClient::execute_with_retry(kDownloadFileMaxRetry, 1, get_file_stat_cb));

            remote_files[filename] = RemoteFileStat {remote_file_url, file_md5, file_size};
        }

        // Step 4: Compare local and remote files && get all need download files
        RETURN_IF_ERROR(_report_every(10, &report_counter, finished_num, total_num,
                                      TTaskType::type::DOWNLOAD));

        // get all need download files
        std::vector<std::string> need_download_files;
        for (const auto& [remote_file, remote_filestat] : remote_files) {
            LOG(INFO) << "remote file: " << remote_file << ", size: " << remote_filestat.size
                      << ", md5: " << remote_filestat.md5;
            auto it = local_files.find(remote_file);
            if (it == local_files.end()) {
                need_download_files.emplace_back(remote_file);
                continue;
            }
            if (_end_with(remote_file, ".hdr")) {
                need_download_files.emplace_back(remote_file);
                continue;
            }

            if (auto& local_filestat = it->second; local_filestat.size != remote_filestat.size) {
                need_download_files.emplace_back(remote_file);
                continue;
            }

            if (auto& local_filestat = it->second; local_filestat.md5 != remote_filestat.md5) {
                need_download_files.emplace_back(remote_file);
                continue;
            }

            LOG(INFO) << fmt::format("file {} already exists, skip download", remote_file);
        }

        auto local_tablet_id = remote_tablet_snapshot.local_tablet_id;
        TabletSharedPtr tablet = _engine.tablet_manager()->get_tablet(local_tablet_id);
        if (tablet == nullptr) {
            std::stringstream ss;
            ss << "failed to get local tablet: " << local_tablet_id;
            LOG(WARNING) << ss.str();
            return Status::InternalError(ss.str());
        }
        DataDir* data_dir = tablet->data_dir();

        // download all need download files
        uint64_t total_file_size = 0;
        MonotonicStopWatch watch;
        watch.start();
        for (auto& filename : need_download_files) {
            auto& remote_filestat = remote_files[filename];
            auto file_size = remote_filestat.size;
            auto& remote_file_url = remote_filestat.url;
            auto& remote_file_md5 = remote_filestat.md5;

            // check disk capacity
            if (data_dir->reach_capacity_limit(file_size)) {
                return Status::Error<ErrorCode::EXCEEDED_LIMIT>(
                        "reach the capacity limit of path {}, file_size={}", data_dir->path(),
                        file_size);
            }

            total_file_size += file_size;
            uint64_t estimate_timeout = file_size / config::download_low_speed_limit_kbps / 1024;
            if (estimate_timeout < config::download_low_speed_time) {
                estimate_timeout = config::download_low_speed_time;
            }

            std::string local_filename;
            RETURN_IF_ERROR(_replace_tablet_id(filename, local_tablet_id, &local_filename));
            std::string local_file_path = local_path + "/" + local_filename;

            LOG(INFO) << "clone begin to download file from: " << remote_file_url
                      << " to: " << local_file_path << ". size(B): " << file_size
                      << ", timeout(s): " << estimate_timeout;

            auto download_cb = [&remote_file_url, &remote_file_md5, estimate_timeout,
                                &local_file_path, file_size](HttpClient* client) {
                RETURN_IF_ERROR(client->init(remote_file_url));
                client->set_timeout_ms(estimate_timeout * 1000);
                RETURN_IF_ERROR(client->download(local_file_path));

                std::error_code ec;
                // Check file length
                uint64_t local_file_size = std::filesystem::file_size(local_file_path, ec);
                if (ec) {
                    LOG(WARNING) << "download file error" << ec.message();
                    return Status::IOError("can't retrive file_size of {}, due to {}",
                                           local_file_path, ec.message());
                }
                if (local_file_size != file_size) {
                    LOG(WARNING) << "download file length error"
                                 << ", remote_path=" << remote_file_url
                                 << ", file_size=" << file_size
                                 << ", local_file_size=" << local_file_size;
                    return Status::InternalError("downloaded file size is not equal");
                }

                if (!remote_file_md5.empty()) { // keep compatibility
                    std::string local_file_md5;
                    RETURN_IF_ERROR(io::global_local_filesystem()->md5sum(local_file_path,
                                                                          &local_file_md5));
                    if (local_file_md5 != remote_file_md5) {
                        LOG(WARNING) << "download file md5 error"
                                     << ", remote_file_url=" << remote_file_url
                                     << ", local_file_path=" << local_file_path
                                     << ", remote_file_md5=" << remote_file_md5
                                     << ", local_file_md5=" << local_file_md5;
                        return Status::RuntimeError(
                                "download file {} md5 is not equal, local={}, remote={}",
                                remote_file_url, local_file_md5, remote_file_md5);
                    }
                }

                return io::global_local_filesystem()->permission(
                        local_file_path, io::LocalFileSystem::PERMS_OWNER_RW);
            };
            auto status = HttpClient::execute_with_retry(kDownloadFileMaxRetry, 1, download_cb);
            if (!status.ok()) {
                LOG(WARNING) << "failed to download file from " << remote_file_url
                             << ", status: " << status.to_string();
                return status;
            }

            // local_files always keep the updated local files
            local_files[filename] = LocalFileStat {file_size, remote_file_md5};
        }

        uint64_t total_time_ms = watch.elapsed_time() / 1000 / 1000;
        total_time_ms = total_time_ms > 0 ? total_time_ms : 0;
        double copy_rate = 0.0;
        if (total_time_ms > 0) {
            copy_rate = total_file_size / ((double)total_time_ms) / 1000;
        }
        auto remote_tablet_id = remote_tablet_snapshot.remote_tablet_id;
        LOG(INFO) << fmt::format(
                "succeed to copy remote tablet {} to local tablet {}, total file size: {} B, cost: "
                "{} ms, rate: {} MB/s",
                remote_tablet_id, local_tablet_id, total_file_size, total_time_ms, copy_rate);

        // local_files: contain all remote files and local files
        // finally, delete local files which are not in remote
        for (const auto& [local_file, local_filestat] : local_files) {
            // replace the tablet id in local file name with the remote tablet id,
            // in order to compare the file name.
            std::string new_name;
            Status st = _replace_tablet_id(local_file, remote_tablet_id, &new_name);
            if (!st.ok()) {
                LOG(WARNING) << "failed to replace tablet id. unknown local file: " << st
                             << ". ignore it";
                continue;
            }
            VLOG_CRITICAL << "new file name after replace tablet id: " << new_name;
            const auto& find = remote_files.find(new_name);
            if (find != remote_files.end()) {
                continue;
            }

            // delete
            std::string full_local_file = local_path + "/" + local_file;
            LOG(INFO) << "begin to delete local snapshot file: " << full_local_file
                      << ", it does not exist in remote";
            if (remove(full_local_file.c_str()) != 0) {
                LOG(WARNING) << "failed to delete unknown local file: " << full_local_file
                             << ", error: " << strerror(errno)
                             << ", file size: " << local_filestat.size << ", ignore it";
            }
        }

        ++finished_num;
    }

    LOG(INFO) << "finished to download snapshots. job: " << _job_id << ", task id: " << _task_id;
    return status;
}

// move the snapshot files in snapshot_path
// to tablet_path
// If overwrite, just replace the tablet_path with snapshot_path,
// else: (TODO)
//
// MUST hold tablet's header lock, push lock, cumulative lock and base compaction lock
Status SnapshotLoader::move(const std::string& snapshot_path, TabletSharedPtr tablet,
                            bool overwrite) {
    // Take a lock to protect the local snapshot path.
    auto local_snapshot_guard = LocalSnapshotLock::instance().acquire(snapshot_path);

    auto tablet_path = tablet->tablet_path();
    auto store_path = tablet->data_dir()->path();
    LOG(INFO) << "begin to move snapshot files. from: " << snapshot_path << ", to: " << tablet_path
              << ", store: " << store_path << ", job: " << _job_id << ", task id: " << _task_id;

    Status status = Status::OK();

    // validate snapshot_path and tablet_path
    int64_t snapshot_tablet_id = 0;
    int32_t snapshot_schema_hash = 0;
    RETURN_IF_ERROR(_get_tablet_id_and_schema_hash_from_file_path(
            snapshot_path, &snapshot_tablet_id, &snapshot_schema_hash));

    int64_t tablet_id = 0;
    int32_t schema_hash = 0;
    RETURN_IF_ERROR(
            _get_tablet_id_and_schema_hash_from_file_path(tablet_path, &tablet_id, &schema_hash));

    if (tablet_id != snapshot_tablet_id || schema_hash != snapshot_schema_hash) {
        std::stringstream ss;
        ss << "path does not match. snapshot: " << snapshot_path
           << ", tablet path: " << tablet_path;
        LOG(WARNING) << ss.str();
        return Status::InternalError(ss.str());
    }

    DataDir* store = _engine.get_store(store_path);
    if (store == nullptr) {
        std::stringstream ss;
        ss << "failed to get store by path: " << store_path;
        LOG(WARNING) << ss.str();
        return Status::InternalError(ss.str());
    }

    if (!std::filesystem::exists(tablet_path)) {
        std::stringstream ss;
        ss << "tablet path does not exist: " << tablet_path;
        LOG(WARNING) << ss.str();
        return Status::InternalError(ss.str());
    }

    if (!std::filesystem::exists(snapshot_path)) {
        std::stringstream ss;
        ss << "snapshot path does not exist: " << snapshot_path;
        LOG(WARNING) << ss.str();
        return Status::InternalError(ss.str());
    }

    // rename the rowset ids and tabletid info in rowset meta
    auto res = _engine.snapshot_mgr()->convert_rowset_ids(snapshot_path, tablet_id,
                                                          tablet->replica_id(), tablet->table_id(),
                                                          tablet->partition_id(), schema_hash);
    if (!res.has_value()) [[unlikely]] {
        auto err_msg =
                fmt::format("failed to convert rowsetids in snapshot: {}, tablet path: {}, err: {}",
                            snapshot_path, tablet_path, res.error());
        LOG(WARNING) << err_msg;
        return Status::InternalError(err_msg);
    }

    if (!overwrite) {
        throw Exception(Status::FatalError("only support overwrite now"));
    }

    // Medium migration/clone/checkpoint/compaction may change or check the
    // files and tablet meta, so we need to take these locks.
    std::unique_lock migration_lock(tablet->get_migration_lock(), std::try_to_lock);
    std::unique_lock base_compact_lock(tablet->get_base_compaction_lock(), std::try_to_lock);
    std::unique_lock cumu_compact_lock(tablet->get_cumulative_compaction_lock(), std::try_to_lock);
    std::unique_lock cold_compact_lock(tablet->get_cold_compaction_lock(), std::try_to_lock);
    std::unique_lock build_idx_lock(tablet->get_build_inverted_index_lock(), std::try_to_lock);
    std::unique_lock meta_store_lock(tablet->get_meta_store_lock(), std::try_to_lock);
    if (!migration_lock.owns_lock() || !base_compact_lock.owns_lock() ||
        !cumu_compact_lock.owns_lock() || !cold_compact_lock.owns_lock() ||
        !build_idx_lock.owns_lock() || !meta_store_lock.owns_lock()) {
        // This error should be retryable
        auto status = Status::ObtainLockFailed("failed to get tablet locks, tablet: {}", tablet_id);
        LOG(WARNING) << status << ", snapshot path: " << snapshot_path
                     << ", tablet path: " << tablet_path;
        return status;
    }

    std::vector<std::string> snapshot_files;
    RETURN_IF_ERROR(_get_existing_files_from_local(snapshot_path, &snapshot_files));

    // FIXME: the below logic will demage the tablet files if failed in the middle.

    // 1. simply delete the old dir and replace it with the snapshot dir
    try {
        // This remove seems soft enough, because we already get
        // tablet id and schema hash from this path, which
        // means this path is a valid path.
        std::filesystem::remove_all(tablet_path);
        VLOG_CRITICAL << "remove dir: " << tablet_path;
        std::filesystem::create_directory(tablet_path);
        VLOG_CRITICAL << "re-create dir: " << tablet_path;
    } catch (const std::filesystem::filesystem_error& e) {
        std::stringstream ss;
        ss << "failed to move tablet path: " << tablet_path << ". err: " << e.what();
        LOG(WARNING) << ss.str();
        return Status::InternalError(ss.str());
    }

    // link files one by one
    // files in snapshot dir will be moved in snapshot clean process
    std::vector<std::string> linked_files;
    for (auto& file : snapshot_files) {
        auto full_src_path = fmt::format("{}/{}", snapshot_path, file);
        auto full_dest_path = fmt::format("{}/{}", tablet_path, file);
        if (link(full_src_path.c_str(), full_dest_path.c_str()) != 0) {
            LOG(WARNING) << "failed to link file from " << full_src_path << " to " << full_dest_path
                         << ", err: " << std::strerror(errno);

            // clean the already linked files
            for (auto& linked_file : linked_files) {
                remove(linked_file.c_str());
            }

            return Status::InternalError("move tablet failed");
        }
        linked_files.push_back(full_dest_path);
        VLOG_CRITICAL << "link file from " << full_src_path << " to " << full_dest_path;
    }

    // snapshot loader not need to change tablet uid
    // fixme: there is no header now and can not call load_one_tablet here
    // reload header
    Status ost = _engine.tablet_manager()->load_tablet_from_dir(store, tablet_id, schema_hash,
                                                                tablet_path, true);
    if (!ost.ok()) {
        std::stringstream ss;
        ss << "failed to reload header of tablet: " << tablet_id;
        LOG(WARNING) << ss.str();
        return Status::InternalError(ss.str());
    }
    LOG(INFO) << "finished to reload header of tablet: " << tablet_id;

    return status;
}

Status SnapshotLoader::_get_tablet_id_and_schema_hash_from_file_path(const std::string& src_path,
                                                                     int64_t* tablet_id,
                                                                     int32_t* schema_hash) {
    // path should be like: /path/.../tablet_id/schema_hash
    // we try to extract tablet_id from path
    size_t pos = src_path.find_last_of("/");
    if (pos == std::string::npos || pos == src_path.length() - 1) {
        return Status::InternalError("failed to get tablet id from path: {}", src_path);
    }

    std::string schema_hash_str = src_path.substr(pos + 1);
    std::stringstream ss1;
    ss1 << schema_hash_str;
    ss1 >> *schema_hash;

    // skip schema hash part
    size_t pos2 = src_path.find_last_of("/", pos - 1);
    if (pos2 == std::string::npos) {
        return Status::InternalError("failed to get tablet id from path: {}", src_path);
    }

    std::string tablet_str = src_path.substr(pos2 + 1, pos - pos2);
    std::stringstream ss2;
    ss2 << tablet_str;
    ss2 >> *tablet_id;

    VLOG_CRITICAL << "get tablet id " << *tablet_id << ", schema hash: " << *schema_hash
                  << " from path: " << src_path;
    return Status::OK();
}

Status SnapshotLoader::_check_local_snapshot_paths(
        const std::map<std::string, std::string>& src_to_dest_path, bool check_src) {
    bool res = true;
    for (const auto& pair : src_to_dest_path) {
        std::string path;
        if (check_src) {
            path = pair.first;
        } else {
            path = pair.second;
        }

        RETURN_IF_ERROR(io::global_local_filesystem()->is_directory(path, &res));
        if (!res) {
            std::stringstream ss;
            ss << "snapshot path is not directory or does not exist: " << path;
            LOG(WARNING) << ss.str();
            return Status::RuntimeError(ss.str());
        }
    }
    LOG(INFO) << "all local snapshot paths are existing. num: " << src_to_dest_path.size();
    return Status::OK();
}

Status SnapshotLoader::_get_existing_files_from_local(const std::string& local_path,
                                                      std::vector<std::string>* local_files) {
    bool exists = true;
    std::vector<io::FileInfo> files;
    RETURN_IF_ERROR(io::global_local_filesystem()->list(local_path, true, &files, &exists));
    for (auto& file : files) {
        local_files->push_back(file.file_name);
    }
    LOG(INFO) << "finished to list files in local path: " << local_path
              << ", file num: " << local_files->size();
    return Status::OK();
}

Status SnapshotLoader::_replace_tablet_id(const std::string& file_name, int64_t tablet_id,
                                          std::string* new_file_name) {
    // eg:
    // 10007.hdr
    // 10007_2_2_0_0.idx
    // 10007_2_2_0_0.dat
    if (_end_with(file_name, ".hdr")) {
        std::stringstream ss;
        ss << tablet_id << ".hdr";
        *new_file_name = ss.str();
        return Status::OK();
    } else if (_end_with(file_name, ".idx") || _end_with(file_name, ".dat")) {
        *new_file_name = file_name;
        return Status::OK();
    } else {
        return Status::InternalError("invalid tablet file name: {}", file_name);
    }
}

Status SnapshotLoader::_get_tablet_id_from_remote_path(const std::string& remote_path,
                                                       int64_t* tablet_id) {
    // eg:
    // bos://xxx/../__tbl_10004/__part_10003/__idx_10004/__10005
    size_t pos = remote_path.find_last_of("_");
    if (pos == std::string::npos) {
        return Status::InternalError("invalid remove file path: {}", remote_path);
    }

    std::string tablet_id_str = remote_path.substr(pos + 1);
    std::stringstream ss;
    ss << tablet_id_str;
    ss >> *tablet_id;

    return Status::OK();
}

// only return CANCELLED if FE return that job is cancelled.
// otherwise, return OK
Status SnapshotLoader::_report_every(int report_threshold, int* counter, int32_t finished_num,
                                     int32_t total_num, TTaskType::type type) {
    ++*counter;
    if (*counter <= report_threshold) {
        return Status::OK();
    }

    LOG(INFO) << "report to frontend. job id: " << _job_id << ", task id: " << _task_id
              << ", finished num: " << finished_num << ", total num:" << total_num;

    TNetworkAddress master_addr = _env->cluster_info()->master_fe_addr;

    TSnapshotLoaderReportRequest request;
    request.job_id = _job_id;
    request.task_id = _task_id;
    request.task_type = type;
    request.__set_finished_num(finished_num);
    request.__set_total_num(total_num);
    TStatus report_st;

    Status rpcStatus = ThriftRpcHelper::rpc<FrontendServiceClient>(
            master_addr.hostname, master_addr.port,
            [&request, &report_st](FrontendServiceConnection& client) {
                client->snapshotLoaderReport(report_st, request);
            },
            10000);

    if (!rpcStatus.ok()) {
        // rpc failed, ignore
        return Status::OK();
    }

    // reset
    *counter = 0;
    if (report_st.status_code == TStatusCode::CANCELLED) {
        LOG(INFO) << "job is cancelled. job id: " << _job_id << ", task id: " << _task_id;
        return Status::Cancelled("Cancelled");
    }
    return Status::OK();
}

Status SnapshotLoader::_list_with_checksum(const std::string& dir,
                                           std::map<std::string, FileStat>* md5_files) {
    bool exists = true;
    std::vector<io::FileInfo> files;
    RETURN_IF_ERROR(_remote_fs->list(dir, true, &files, &exists));
    for (auto& tmp_file : files) {
        io::Path path(tmp_file.file_name);
        std::string file_name = path.filename();
        size_t pos = file_name.find_last_of(".");
        if (pos == std::string::npos || pos == file_name.size() - 1) {
            // Not found checksum separator, ignore this file
            continue;
        }
        FileStat stat = {std::string(file_name, 0, pos), std::string(file_name, pos + 1),
                         tmp_file.file_size};
        md5_files->emplace(std::string(file_name, 0, pos), stat);
    }

    return Status::OK();
}

} // end namespace doris
