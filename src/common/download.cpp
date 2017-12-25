// Copyright (c) 2017, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <string>
#include <atomic>
#include <boost/filesystem.hpp>
#include <boost/asio.hpp>
#include <boost/thread/thread.hpp>
#include "cryptonote_config.h"
#include "include_base_utils.h"
#include "net/http_client.h"
#include "download.h"

#undef EDOLLAR_DEFAULT_LOG_CATEGORY
#define EDOLLAR_DEFAULT_LOG_CATEGORY "net.dl"

namespace tools
{
  struct download_thread_control
  {
    const std::string path;
    const std::string uri;
    std::function<void(const std::string&, const std::string&, bool)> result_cb;
    std::function<bool(const std::string&, const std::string&, size_t, ssize_t)> progress_cb;
    bool stop;
    bool stopped;
    bool success;
    boost::thread thread;
    boost::mutex mutex;

    download_thread_control(const std::string &path, const std::string &uri, std::function<void(const std::string&, const std::string&, bool)> result_cb, std::function<bool(const std::string&, const std::string&, size_t, ssize_t)> progress_cb):
        path(path), uri(uri), result_cb(result_cb), progress_cb(progress_cb), stop(false), stopped(false), success(false) {}
    ~download_thread_control() { if (thread.joinable()) thread.detach(); }
  };

  static void download_thread(download_async_handle control)
  {
    static std::atomic<unsigned int> thread_id(0);

    MLOG_SET_THREAD_NAME("DL" + std::to_string(thread_id++));

    struct stopped_setter
    {
      stopped_setter(const download_async_handle &control): control(control) {}
      ~stopped_setter() { control->stopped = true; }
      download_async_handle control;
    } stopped_setter(control);

    try
    {
      boost::unique_lock<boost::mutex> lock(control->mutex);
      MINFO("Downloading " << control->uri << " to " << control->path);
      std::ofstream f;
      f.open(control->path, std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
      if (!f.good()) {
        MERROR("Failed to open file " << control->path);
        control->result_cb(control->path, control->uri, control->success);
        return;
      }
      class download_client: public epee::net_utils::http::http_simple_client
      {
      public:
        download_client(download_async_handle control, std::ofstream &f):
          control(control), f(f), content_length(-1), total(0) {}
        virtual ~download_client() { f.close(); }
        virtual bool on_header(const epee::net_utils::http::http_response_info &headers)
        {
          ssize_t length;
          if (epee::string_tools::get_xtype_from_string(length, headers.m_header_info.m_content_length) && length >= 0)
          {
            MINFO("Content-Length: " << length);
            content_length = length;
            boost::filesystem::path path(control->path);
            boost::filesystem::space_info si = boost::filesystem::space(path);
            if (si.available < (size_t)content_length)
            {
              const uint64_t avail = (si.available + 1023) / 1024, needed = (content_length + 1023) / 1024;
              MERROR("Not enough space to download " << needed << " kB to " << path << " (" << avail << " kB available)");
              return false;
            }
          }
          return true;
        }
        virtual bool handle_target_data(std::string &piece_of_transfer)
        {
          try
          {
            boost::lock_guard<boost::mutex> lock(control->mutex);
            if (control->stop)
              return false;
            f << piece_of_transfer;
            total += piece_of_transfer.size();
            if (control->progress_cb && !control->progress_cb(control->path, control->uri, total, content_length))
              return false;
            return f.good();
          }
          catch (const std::exception &e)
          {
            MERROR("Error writing data: " << e.what());
            return false;
          }
        }
      private:
        download_async_handle control;
        std::ofstream &f;
        ssize_t content_length;
        size_t total;
      } client(control, f);
      epee::net_utils::http::url_content u_c;
      if (!epee::net_utils::parse_url(control->uri, u_c))
      {
        MERROR("Failed to parse URL " << control->uri);
        control->result_cb(control->path, control->uri, control->success);
        return;
      }
      if (u_c.host.empty())
      {
        MERROR("Failed to determine address from URL " << control->uri);
        control->result_cb(control->path, control->uri, control->success);
        return;
      }

      lock.unlock();

      uint16_t port = u_c.port ? u_c.port : 80;
      MDEBUG("Connecting to " << u_c.host << ":" << port);
      client.set_server(u_c.host, std::to_string(port), boost::none);
      if (!client.connect(std::chrono::seconds(30)))
      {
        boost::lock_guard<boost::mutex> lock(control->mutex);
        MERROR("Failed to connect to " << control->uri);
        control->result_cb(control->path, control->uri, control->success);
        return;
      }
      MDEBUG("GETting " << u_c.uri);
      const epee::net_utils::http::http_response_info *info = NULL;
      if (!client.invoke_get(u_c.uri, std::chrono::seconds(30), "", &info))
      {
        boost::lock_guard<boost::mutex> lock(control->mutex);
        MERROR("Failed to connect to " << control->uri);
        client.disconnect();
        control->result_cb(control->path, control->uri, control->success);
        return;
      }
      if (control->stop)
      {
        boost::lock_guard<boost::mutex> lock(control->mutex);
        MDEBUG("Download cancelled");
        client.disconnect();
        control->result_cb(control->path, control->uri, control->success);
        return;
      }
      if (!info)
      {
        boost::lock_guard<boost::mutex> lock(control->mutex);
        MERROR("Failed invoking GET command to " << control->uri << ", no status info returned");
        client.disconnect();
        control->result_cb(control->path, control->uri, control->success);
        return;
      }
      MDEBUG("response code: " << info->m_response_code);
      MDEBUG("response length: " << info->m_header_info.m_content_length);
      MDEBUG("response comment: " << info->m_response_comment);
      MDEBUG("response body: " << info->m_body);
      for (const auto &f: info->m_additional_fields)
        MDEBUG("additional field: " << f.first << ": " << f.second);
      if (info->m_response_code != 200)
      {
        boost::lock_guard<boost::mutex> lock(control->mutex);
        MERROR("Status code " << info->m_response_code);
        client.disconnect();
        control->result_cb(control->path, control->uri, control->success);
        return;
      }
      client.disconnect();
      f.close();
      MDEBUG("Download complete");
      lock.lock();
      control->success = true;
      control->result_cb(control->path, control->uri, control->success);
      return;
    }
    catch (const std::exception &e)
    {
      MERROR("Exception in download thread: " << e.what());
      // fall through and call result_cb not from the catch block to avoid another exception
    }
    boost::lock_guard<boost::mutex> lock(control->mutex);
    control->result_cb(control->path, control->uri, control->success);
  }

  bool download(const std::string &path, const std::string &url, std::function<bool(const std::string&, const std::string&, size_t, ssize_t)> cb)
  {
    bool success = false;
    download_async_handle handle = download_async(path, url, [&success](const std::string&, const std::string&, bool result) {success = result;}, cb);
    download_wait(handle);
    return success;
  }

  download_async_handle download_async(const std::string &path, const std::string &url, std::function<void(const std::string&, const std::string&, bool)> result, std::function<bool(const std::string&, const std::string&, size_t, ssize_t)> progress)
  {
    download_async_handle control = std::make_shared<download_thread_control>(path, url, result, progress);
    control->thread = boost::thread([control](){ download_thread(control); });
    return control;
  }

  bool download_finished(const download_async_handle &control)
  {
    CHECK_AND_ASSERT_MES(control != 0, false, "NULL async download handle");
    boost::lock_guard<boost::mutex> lock(control->mutex);
    return control->stopped;
  }

  bool download_error(const download_async_handle &control)
  {
    CHECK_AND_ASSERT_MES(control != 0, false, "NULL async download handle");
    boost::lock_guard<boost::mutex> lock(control->mutex);
    return !control->success;
  }

  bool download_wait(const download_async_handle &control)
  {
    CHECK_AND_ASSERT_MES(control != 0, false, "NULL async download handle");
    {
      boost::lock_guard<boost::mutex> lock(control->mutex);
      if (control->stopped)
        return true;
    }
    control->thread.join();
    return true;
  }

  bool download_cancel(const download_async_handle &control)
  {
    CHECK_AND_ASSERT_MES(control != 0, false, "NULL async download handle");
    {
      boost::lock_guard<boost::mutex> lock(control->mutex);
      if (control->stopped)
        return true;
      control->stop = true;
    }
    control->thread.join();
    return true;
  }
}
