/*MT*
    
    MediaTomb - http://www.mediatomb.cc/
    
    transcode_ext_handler.cc - this file is part of MediaTomb.
    
    Copyright (C) 2005 Gena Batyan <bgeradz@mediatomb.cc>,
                       Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>
    
    Copyright (C) 2006-2010 Gena Batyan <bgeradz@mediatomb.cc>,
                            Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>,
                            Leonhard Wimmer <leo@mediatomb.cc>
    
    MediaTomb is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation.
    
    MediaTomb is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    version 2 along with MediaTomb; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
    
    $Id$
*/

/// \file transcode_ext_handler.cc

#include "transcode_ext_handler.h"
#include "server.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ixml.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <csignal>
#include <climits>
#include "common.h"
#include "config/config_manager.h"
#include "storage/storage.h"
#include "cds_objects.h"
#include "util/process.h"
#include "update_manager.h"
#include "web/session_manager.h"
#include "iohandler/process_io_handler.h"
#include "iohandler/buffered_io_handler.h"
#include "iohandler/file_io_handler.h"
#include "iohandler/io_handler_chainer.h"
#include "metadata/metadata_handler.h"
#include "util/tools.h"
#include "transcoding_process_executor.h"
#include "iohandler/io_handler_chainer.h"
#include "content_manager.h"

#ifdef HAVE_CURL
    #include "iohandler/curl_io_handler.h"
#endif

using namespace zmm;

TranscodeExternalHandler::TranscodeExternalHandler(std::shared_ptr<ConfigManager> config,
    std::shared_ptr<ContentManager> content)
    : TranscodeHandler(config, content)
{
}

std::unique_ptr<IOHandler> TranscodeExternalHandler::open(Ref<TranscodingProfile> profile,
    std::string location,
    std::shared_ptr<CdsObject> obj,
    std::string range)
{
    bool isURL = false;
//    bool is_srt = false;

    log_debug("start transcoding file: {}", location.c_str());
    char fifo_template[]="mt_transcode_XXXXXX";

    if (profile == nullptr)
        throw std::runtime_error("Transcoding of file " + location +
                         "requested but no profile given");
    
    isURL = (IS_CDS_ITEM_INTERNAL_URL(obj->getObjectType()) ||
            IS_CDS_ITEM_EXTERNAL_URL(obj->getObjectType()));

    std::string mimeType = profile->getTargetMimeType();

    if (IS_CDS_ITEM(obj->getObjectType()))
    {
        auto item = std::static_pointer_cast<CdsItem>(obj);
        auto mappings = config->getDictionaryOption(
                CFG_IMPORT_MAPPINGS_MIMETYPE_TO_CONTENTTYPE_LIST);

        if (getValueOrDefault(mappings, mimeType) == CONTENT_TYPE_PCM)
        {
            std::string freq = item->getResource(0)->getAttribute(MetadataHandler::getResAttrName(R_SAMPLEFREQUENCY));
            std::string nrch = item->getResource(0)->getAttribute(MetadataHandler::getResAttrName(R_NRAUDIOCHANNELS));

            if (string_ok(freq)) 
                mimeType = mimeType + ";rate=" + freq;
            if (string_ok(nrch))
                mimeType = mimeType + ";channels=" + nrch;
        }
    }

    /* Upstream, move to getinfo?
    info->content_type = ixmlCloneDOMString(mimeType.c_str());
#ifdef EXTEND_PROTOCOLINFO
    std::string header;
    header = header + "TimeSeekRange.dlna.org: npt=" + range;

    log_debug("Adding TimeSeekRange response HEADERS: {}", header.c_str());
    header = getDLNAtransferHeader(mimeType, header);
    if (string_ok(header))
        info->http_header = ixmlCloneDOMString(header.c_str());
#endif

    info->file_length = UNKNOWN_CONTENT_LENGTH;
    info->force_chunked = (int)profile->getChunked();
    */

    std::string fifo_name = normalizePath(tempName(config->getOption(CFG_SERVER_TMPDIR),
                                     fifo_template));
    std::string arguments;
    std::string temp;
    std::string command;
    std::vector<std::string> arglist;
    std::vector<std::shared_ptr<ProcListItem>> proc_list;

#ifdef SOPCAST
    service_type_t service = OS_None;
    if (obj->getFlag(OBJECT_FLAG_ONLINE_SERVICE))
    {
        service = (service_type_t)std::stoi(obj->getAuxData(ONLINE_SERVICE_AUX_ID));
    }
    
    if (service == OS_SopCast)
    {
        std::vector<std::string> sop_args;
        int p1 = find_local_port(45000,65500);
        int p2 = find_local_port(45000,65500);
        sop_args = populateCommandLine(location + " " + std::to_string(p1) + " " + std::to_string(p2), nullptr, nullptr, nullptr);
        auto spsc = std::make_shared<ProcessExecutor>("sp-sc-auth", sop_args);
        auto pr_item = std::make_shared<ProcListItem>(spsc);
        proc_list.push_back(pr_item);
        location = "http://localhost:" + std::to_string(p2) + "/tv.asf";

//FIXME: #warning check if socket is ready
        sleep(15); 
    }
//FIXME: #warning check if we can use "accept url" with sopcast
    else
    {
#endif
        if (isURL && (!profile->acceptURL()))
        {
#ifdef HAVE_CURL
            std::string url = location;
            strcpy(fifo_template, "mt_transcode_XXXXXX");
            location = normalizePath(tempName(config->getOption(CFG_SERVER_TMPDIR), fifo_template));
            log_debug("creating reader fifo: {}", location.c_str());
            if (mkfifo(location.c_str(), O_RDWR) == -1)
            {
                log_error("Failed to create fifo for the remote content "
                          "reading thread: {}\n", strerror(errno));
                throw std::runtime_error("Could not create reader fifo!\n");
            }

            try
            {
                chmod(location.c_str(), S_IWUSR | S_IRUSR);

                std::unique_ptr<IOHandler> c_ioh = std::make_unique<CurlIOHandler>(url, nullptr,
                   config->getIntOption(CFG_EXTERNAL_TRANSCODING_CURL_BUFFER_SIZE),
                   config->getIntOption(CFG_EXTERNAL_TRANSCODING_CURL_FILL_SIZE));
                std::unique_ptr<IOHandler> p_ioh = std::make_unique<ProcessIOHandler>(content, location, nullptr);
                auto ch = std::make_shared<IOHandlerChainer>(c_ioh, p_ioh, 16384);
                auto pr_item = std::make_shared<ProcListItem>(ch);
                proc_list.push_back(pr_item);
            }
            catch (const std::runtime_error& ex)
            {
                unlink(location.c_str());
                throw ex;
            }
#else
            throw std::runtime_error("MediaTomb was compiled without libcurl support,"
                             "data proxying is not available");
#endif

        }
#ifdef SOPCAST
    }
#endif

    std::string check;
    if (startswith(profile->getCommand(), _DIR_SEPARATOR))
    {
        if (!check_path(profile->getCommand()))
            throw std::runtime_error("Could not find transcoder: " +
                    profile->getCommand());

        check = profile->getCommand();
    }
    else
    {
        check = find_in_path(profile->getCommand());

        if (!string_ok(check))
            throw std::runtime_error("Could not find transcoder " +
                        profile->getCommand() + " in $PATH");

    }

    int err = 0;
    if (!is_executable(check, &err))
        throw std::runtime_error("Transcoder " + profile->getCommand() +
                " is not executable: " + strerror(err));

    log_debug("creating fifo: {}", fifo_name.c_str());
    if (mkfifo(fifo_name.c_str(), O_RDWR) == -1) 
    {
        log_error("Failed to create fifo for the transcoding process!: {}", strerror(errno));
        throw std::runtime_error("Could not create fifo!\n");
    }
        
    chmod(fifo_name.c_str(), S_IWUSR | S_IRUSR);
   
    arglist = populateCommandLine(profile->getArguments(), location, fifo_name, range);

    log_debug("Command: {}", profile->getCommand().c_str());
    log_debug("Arguments: {}", profile->getArguments().c_str());
    auto main_proc = std::make_shared<TranscodingProcessExecutor>(profile->getCommand(), arglist);
    main_proc->removeFile(fifo_name);
    if (isURL && (!profile->acceptURL()))
    {
        main_proc->removeFile(location);
    }

    std::unique_ptr<IOHandler> u_ioh = std::make_unique<ProcessIOHandler>(content, fifo_name, main_proc, proc_list);
    auto io_handler = std::make_unique<BufferedIOHandler>(
        u_ioh,
        profile->getBufferSize(), profile->getBufferChunkSize(), profile->getBufferInitialFillSize()
    );
    io_handler->open(UPNP_READ);
    content->triggerPlayHook(obj);
    return io_handler;
}
