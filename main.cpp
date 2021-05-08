#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

#include <chrono>

//#define parallel_cfg_omp

#ifndef parallel_cfg_omp
#define parallel_cfg_std_thread
#include <thread>
#else
#include <omp.h>
#endif

namespace fs = std::filesystem;

void processFile(std::filesystem::path p, const std::vector<std::pair<std::string, std::string>> replacing_table);

int main()
{
    auto ms = std::chrono::duration_cast< std::chrono::milliseconds >(
            std::chrono::system_clock::now().time_since_epoch()
                );
    auto tm_beg = ms.count();

    std::ifstream configuration { "config.ini" };
    if (configuration.fail())
    {
        std::cerr << "Error: config.ini" << ": " << std::strerror(errno) << std::endl;
        return 1;
    }
    std::vector<std::string> cfg_content;
    std::string str_cfg;
    while (!configuration.eof())
    {
        std::getline(configuration, str_cfg);
        if (str_cfg.empty()) continue;
        cfg_content.emplace_back(std::move(str_cfg));
    }
    if (cfg_content.size() < 4) { std::cerr << "Wrong config.ini format\n"; return 2; }

    std::string threadsNum("threads_num:");
    auto pos = cfg_content[0].find(threadsNum);
    if (pos != 0) // не начало строки
    {
        std::cerr << "Wrong config.ini format\n";
        return 2;
    }
    cfg_content[0].replace(pos, threadsNum.size(), "");
#ifdef parallel_cfg_omp
    omp_set_num_threads(std::stoi(cfg_content[0]));
#endif

#ifdef parallel_cfg_std_thread
    const size_t threads_num = std::stoul(cfg_content[0]);
    std::vector<std::thread> work_threads;
#endif

    std::string workPth("work_path:");
    pos = cfg_content[1].find(workPth);
    if (pos != 0) // не начало строки
    {
        std::cerr << "Wrong config.ini format\n";
        return 2;
    }
    cfg_content[1].replace(pos, workPth.size(), "");
    std::string work_path = std::move(cfg_content[1]);
    if (work_path.empty()) { std::cerr << "Empty work path in config.ini\n"; return 3; }
    fs::path wp {work_path};
    if (!fs::exists(wp))
    {
        std::cerr << "Wrong work path in config.ini: " << std::strerror(errno) << std::endl;
        return 4;
    }

    if (cfg_content[2] != "replacing_table:") { std::cerr << "Wrong config.ini format\n"; return 2; }
    std::vector<std::pair<std::string, std::string>> replacing_table;
    for (size_t r = 3; r < cfg_content.size(); ++r)
    {
        if (cfg_content[r].empty()) continue;
        std::istringstream f(cfg_content[r]);
        std::string templ, replc;
        std::getline(f, templ, ';');
        std::getline(f, replc, ';');
        replacing_table.emplace_back(std::move(templ), std::move(replc));
    }

    fs::recursive_directory_iterator begin_it( wp );
    fs::recursive_directory_iterator end_it;
    std::vector<fs::path> files;
    std::vector<fs::path> files_for_changes;

    std::copy_if(begin_it, end_it, std::back_inserter(files),
                 [](const fs::path &p)
    {
        return  fs::is_regular_file(p);
    });

    std::cout << files.size() << " files" << std::endl;

#ifdef parallel_cfg_std_thread

    bool started_thread[threads_num];
    size_t f_idx = 0;
    for (size_t thr_idx = 0; thr_idx < threads_num; ++thr_idx)
    {
        std::thread th_new { processFile, files[f_idx], replacing_table };
        work_threads.push_back(std::move(th_new));
        started_thread[thr_idx] = true;

        ++f_idx;
    }
    while (true)
    {
        for (size_t thr_idx = 0; thr_idx < threads_num && f_idx < files.size(); ++thr_idx)
        {
            if (started_thread[thr_idx])
            {
                if (work_threads[thr_idx].joinable())
                {
                    work_threads[thr_idx].join();
                    started_thread[thr_idx] = false;
                }
                else continue;
            }
            if (!started_thread[thr_idx])
            {
                std::thread th_new { processFile, files[f_idx], replacing_table };
                work_threads[thr_idx] = std::move(th_new);
                started_thread[thr_idx] = true;
                f_idx++;
            }
        }

        if (f_idx >= files.size())
        {
            while (work_threads.size())
            {
                if (work_threads.front().joinable())
                {
                    work_threads.front().join();
                    work_threads.erase(work_threads.begin());
                }
            }

            break;
        }
    }

#else
#ifdef parallel_cfg_omp
#pragma omp parallel for //shared(files, replacing_table)
#endif
    for (size_t f_idx = 0; f_idx < files.size(); ++f_idx)
    {
        processFile(files[f_idx], replacing_table);
    }

#endif

    ms = std::chrono::duration_cast< std::chrono::milliseconds >(
                std::chrono::system_clock::now().time_since_epoch()
                    );
    auto tm_end = ms.count();

    std::cout << "work is finished by " << (tm_end - tm_beg) << " msec\n";

    return 0;
}

void processFile(std::filesystem::path p, const std::vector<std::pair<std::string, std::string> > replacing_table)
{
    std::ifstream in_stream_txt { p.c_str() };

    std::vector<std::string> content;
    std::string str;
    auto found = false;
    while (!in_stream_txt.eof())
    {
        std::getline(in_stream_txt, str);

        for (auto it = replacing_table.cbegin(); it != replacing_table.cend(); ++it)
        {
            std::string tmpl = it->first;
            if (std::regex_search(std::begin(str), std::end(str), std::regex(tmpl)))
            {
                found = true;
                size_t pos = str.find(tmpl);
                while (pos != std::string::npos)
                {
                    str.replace(pos, tmpl.size(), it->second);
                    pos = str.find(tmpl, pos);
                }
            }
        }

        content.emplace_back(std::move(str));
    }
    in_stream_txt.close();

    if (found)
    {
        std::ofstream out_stream_txt { p.c_str() };
        for (auto it = content.cbegin(); it != content.cend(); ++it)
        {
            out_stream_txt << *it << "\n";
        }
        out_stream_txt.close();
    }

    content.clear();
}
