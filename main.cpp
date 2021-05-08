#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <list>
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

void processFiles(std::vector<fs::path> vp, std::vector<std::pair<std::string, std::string>> replacing_table);

int main()
{
    auto ms = std::chrono::duration_cast< std::chrono::milliseconds >(
            std::chrono::system_clock::now().time_since_epoch()
                );
    auto tm_beg = ms.count();

    /// --------- чтение конфигурации ---------
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
    const size_t threads_num = std::stoul(cfg_content[0]);
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

    /// --------- рекурсивный поиск обычных (regular) файлов в указанном каталоге и его подкаталогах ---------
    fs::recursive_directory_iterator begin_it( wp );
    fs::recursive_directory_iterator end_it;
    std::vector<fs::path> files;

    std::copy_if(begin_it, end_it, std::back_inserter(files),
                 [](const fs::path &p)
    {
        return  fs::is_regular_file(p);
    });

    std::cout << files.size() << " files" << std::endl;

    /// --------- сортировка полученного списка файлов по возрастанию их размера ---------
    std::stable_sort(files.begin(), files.end(),
                     [](const fs::path &a, const fs::path &b)
    {
        return fs::file_size(a) < fs::file_size(b);
    });

#if defined(parallel_cfg_std_thread) || defined(parallel_cfg_omp)

    /// --------- формирование набора порций с разным числом файлов,
    /// но примерно одного суммарного размера (data_for_multithread_work) ---------
    auto min_sz = fs::file_size(files.front());
    auto max_sz = min_sz;
    auto all_sz = min_sz;
    auto it = files.begin();
    ++it;

    for (; it != files.end(); ++it)
    {
        auto sz = fs::file_size(*it);
        all_sz += sz;
        if (sz < min_sz) min_sz = sz;
        if (sz > max_sz) max_sz = sz;
    }

    std::list<int> portions_of_files_for_threads;

    // выбор "рекомендуемого" размера порции, если есть достаточно болшие файлы - этот размер корректируется
    // и приближается к размеру наибольшего файла (последние порции - одна или несколько - в этом случае
    // будут содержать по одному файлу из наибольших)
    auto part_size_hint = all_sz / 40 + all_sz % 40 * 40;
    if (part_size_hint < max_sz)
    {
        if (max_sz < part_size_hint * 2)
        {
            auto delta_ = (max_sz - part_size_hint);
            auto delim = 40;
            while (delta_ * 10 > part_size_hint)
            {
                delim--;
                part_size_hint = all_sz / delim;
                delta_ = (max_sz - part_size_hint);
            }
        }
        else
        {
            auto k = max_sz / part_size_hint;
            part_size_hint *= k;
        }
    }

    auto portion_count = 0;
    auto portion_size_bytes = 0UL;

    for (it = files.begin(); it != files.end(); ++it)
    {
        auto sz_bytes = fs::file_size(*it);
        if (sz_bytes > part_size_hint && portion_count == 0)
        {
            // большой файл в начале порции - займёт всю порцию
            portions_of_files_for_threads.push_back(1);
        }
        else if (sz_bytes > part_size_hint && portion_count > 0)
        {
            // большой файл НЕ в начале порции - займёт всю следующую порцию, текущая неполная порция тоже завершается
            portion_size_bytes = 0UL;
            portions_of_files_for_threads.push_back(portion_count);
            portion_count = 0;
            portions_of_files_for_threads.push_back(1);
        }
        else if (sz_bytes < part_size_hint)
        {
            auto new_portion_size_bytes = portion_size_bytes + sz_bytes;
            if (new_portion_size_bytes <= part_size_hint)
            {
                // новый файл ещё умещается в порции
                portion_size_bytes = new_portion_size_bytes;
                ++portion_count;
            }
            else
            {
                // порция сформирована - файл пойдет в следующую
                portions_of_files_for_threads.push_back(portion_count);
                // файл формирует новую порцию
                portion_count = 1;
                portion_size_bytes = sz_bytes;
            }
        }
    }

    std::vector<std::vector<fs::path>> data_for_multithread_work;
    it = files.begin();
    for (auto it2 = portions_of_files_for_threads.cbegin(); it2 != portions_of_files_for_threads.cend(); ++it2)
    {
        auto it_p = it;
        std::advance(it_p, *it2);
        std::vector<fs::path> subvector_files;
        std::copy(it, it_p, std::back_inserter(subvector_files));
        data_for_multithread_work.push_back(std::move(subvector_files));
        it = it_p;
    }

#endif

#ifdef parallel_cfg_std_thread

    /// --------- поиск в файлах подстрок на замену - одна порция обрабатывается в одном потоке ---------

    bool started_thread[threads_num];
    size_t fg_idx = 0;
    auto more = true;
    for (size_t thr_idx = 0; thr_idx < threads_num; ++thr_idx)
    {
        std::thread th_new { processFiles, data_for_multithread_work[fg_idx], replacing_table };
        work_threads.push_back(std::move(th_new));
        started_thread[thr_idx] = true;

        ++fg_idx;
        if (fg_idx >= data_for_multithread_work.size())
        {
            more = false;
            break;
        }
    }
    if (!more) // число групп (порций) меньше максимального числа потоков - ожидание их остановки
    {
        for (size_t thr_idx = 0; thr_idx < threads_num; ++thr_idx)
        {
            if (started_thread[thr_idx]) work_threads[thr_idx].join();
        }
    }
    while (more)
    {
        for (size_t thr_idx = 0; thr_idx < threads_num && fg_idx < data_for_multithread_work.size(); ++thr_idx)
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
                std::thread th_new { processFiles, data_for_multithread_work[fg_idx], replacing_table };
                work_threads[thr_idx] = std::move(th_new);
                started_thread[thr_idx] = true;
                fg_idx++;
            }
        }

        if (fg_idx >= data_for_multithread_work.size())
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
#pragma omp parallel for // вариант распараллеливания через OpenMP
    for (size_t fg_idx = 0; fg_idx < data_for_multithread_work.size(); ++fg_idx)
    {
        processFiles(data_for_multithread_work[fg_idx], replacing_table);
    }
#else
    processFiles(files, replacing_table); // случай последовательного выполнения
#endif

#endif

    ms = std::chrono::duration_cast< std::chrono::milliseconds >(
                std::chrono::system_clock::now().time_since_epoch()
                    );
    auto tm_end = ms.count();

    std::cout << "work is finished by " << (tm_end - tm_beg) << " msec\n";

    return 0;
}

void processFiles(std::vector<fs::path> vp, std::vector<std::pair<std::string, std::string>> replacing_table)
{
    for (auto p : vp)
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

        if (found) // перезаписать только файлы с измененным содержимым
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
}
