/**
 * Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "pageinfo.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-arith"

#ifndef BUILD_TARGET_TOGETHER
DEFINE_PLUGIN_COMMAND(Pageinfo)
#endif

void Pageinfo::cmd_main(void) {
    int c;
    std::string cppString;
    if (argcnt < 2) cmd_usage(pc->curcmd, SYNOPSIS);
    while ((c = getopt(argcnt, args, "af")) != EOF) {
        switch(c) {
            case 'f':
                print_file_pages();
                break;
            case 'a':
                print_anon_pages();
                break;
            default:
                argerrs++;
                break;
        }
    }
    if (argerrs)
        cmd_usage(pc->curcmd, SYNOPSIS);
}

void Pageinfo::print_anon_pages(){
    for (size_t pfn = min_low_pfn; pfn < max_pfn; pfn++){
        ulong page = pfn_to_page(pfn);
        if (!is_kvaddr(page)){
            continue;
        }
        if(page_buddy(page) || page_count(page) == 0){
            continue;
        }
        ulong mapping = read_pointer(page + field_offset(page,mapping),"mapping");
        if (!is_kvaddr(mapping)){
            continue;
        }
        if(mapping & 0x1 == 0){ // skip file page
            continue;
        }
        physaddr_t paddr = page_to_phy(page);
        fprintf(fp, "page:%#lx  paddr:%#llx \n",page,(ulonglong)paddr);
    }
}

void Pageinfo::parser_file_pages(){
    std::set<ulong> inode_list;
    for (size_t pfn = min_low_pfn; pfn < max_pfn; pfn++){
        ulong page = pfn_to_page(pfn);
        if (!is_kvaddr(page)){
            continue;
        }
        if(page_buddy(page) || page_count(page) == 0){
            continue;
        }
        ulong mapping = read_pointer(page + field_offset(page,mapping),"mapping");
        if (!is_kvaddr(mapping)){
            continue;
        }
        if(mapping & 0x1 == 1){ // skip anon page
            continue;
        }
        ulong inode = read_pointer(mapping + field_offset(address_space,host),"host");
        if (!is_kvaddr(inode)){
            continue;
        }
        ulong ops = read_pointer(mapping + field_offset(address_space,a_ops),"a_ops");
        if (!is_kvaddr(ops)){
            continue;
        }
        ulong i_mapping = read_pointer(inode + field_offset(inode,i_mapping),"i_mapping");
        if (!is_kvaddr(i_mapping) && mapping != i_mapping){
            continue;
        }
        inode_list.insert(inode);
    }
    char *file_buf = nullptr;
    char buf[BUFSIZE];
    for (const auto& addr : inode_list) {
        std::shared_ptr<FileCache> file_ptr = std::make_shared<FileCache>();
        file_ptr->inode = addr;
        ulong hlist_head = addr + field_offset(inode,i_dentry);
        int offset = field_offset(dentry,d_u);
        for (const auto& dentry : for_each_hlist(hlist_head,offset)) {
            get_pathname(dentry, buf, BUFSIZE, 1, 0);
            file_ptr->name = buf;
            if (!file_ptr->name.empty()){
                break;
            }
        }
        file_ptr->i_mapping = read_pointer(addr + field_offset(inode,i_mapping),"i_mapping");
        file_ptr->nrpages = read_ulong(file_ptr->i_mapping + field_offset(address_space, nrpages), "nrpages");
        cache_list.push_back(file_ptr);
    }
}

void Pageinfo::print_file_pages(){
    if (cache_list.size() == 0){
        parser_file_pages();
    }
    int64_t total_size = 0;
    for (const auto& file_ptr : cache_list) {
        total_size += file_ptr->nrpages;
    }
    fprintf(fp, "Total File cache size: %s \n",csize(total_size * page_size).c_str());
    std::sort(cache_list.begin(), cache_list.end(),[&](std::shared_ptr<FileCache> a, std::shared_ptr<FileCache> b){
        return a->nrpages > b->nrpages;
    });
    fprintf(fp, "===============================================\n");
    std::ostringstream oss_hd;
    oss_hd  << std::left << std::setw(VADDR_PRLEN)  << "inode" << " "
            << std::left << std::setw(VADDR_PRLEN)  << "address_space" << " "
            << std::left << std::setw(8)            << "nrpages" << " "
            << std::left << std::setw(10)           << "size" << " "
            << std::left << "Path";
    fprintf(fp, "%s \n",oss_hd.str().c_str());
    for (const auto& file_ptr : cache_list) {
        std::ostringstream oss;
        oss << std::left << std::hex  << std::setw(VADDR_PRLEN) << file_ptr->inode << " "
            << std::left << std::hex  << std::setw(VADDR_PRLEN) << file_ptr->i_mapping << " "
            << std::left << std::dec  << std::setw(8)           << file_ptr->nrpages << " "
            << std::left << std::dec  << std::setw(10)          << csize(file_ptr->nrpages * page_size) << " "
            << std::left << file_ptr->name;
        fprintf(fp, "%s \n",oss.str().c_str());
    }
}

bool Pageinfo::page_buddy(ulong page_addr){
    if (THIS_KERNEL_VERSION >= LINUX(4, 19, 0)){
        uint page_type = read_uint(page_addr + field_offset(page,page_type),"page_type");
        return ((page_type & 0xf0000080) == 0xf0000000);
    }else{
        int mapcount = read_int(page_addr + field_offset(page,_mapcount),"_mapcount");
        return mapcount == 0xffffff80;
    }
}

int Pageinfo::page_count(ulong page_addr){
    int count = 0;
    if (THIS_KERNEL_VERSION < LINUX(4, 6, 0)){
        count = read_int(page_addr + field_offset(page,_count),"_count");
    }else{
        count = read_int(page_addr + field_offset(page,_refcount),"_refcount");
    }
    return count;
}

void Pageinfo::init_offset(){
    field_init(page,page_type);
    field_init(page,_mapcount);
    field_init(page,_count);
    field_init(page,_refcount);
    field_init(page,mapping);
    field_init(address_space,host);
    field_init(address_space,a_ops);
    field_init(address_space, nrpages);
    field_init(inode,i_mapping);
    field_init(inode,i_dentry);
    field_init(inode,i_sb);
    field_init(dentry,d_u);
    /* max_pfn */
    if (csymbol_exists("max_pfn")){
        try_get_symbol_data(TO_CONST_STRING("max_pfn"), sizeof(ulong), &max_pfn);
    }
    /* min_low_pfn */
    if (csymbol_exists("min_low_pfn")){
        try_get_symbol_data(TO_CONST_STRING("min_low_pfn"), sizeof(ulong), &min_low_pfn);
    }
}

Pageinfo::Pageinfo(){
    cmd_name = "cache";
    help_str_list={
        "cache",                            /* command name */
        "dump page information",        /* short description */
        "-f \n"
            "  cache -a\n"
            "  This command dumps the page cache info.",
        "\n",
        "EXAMPLES",
        "  Display all file info:",
        "    %s> cache -f",
        "    Total File cache size: 570.22MB",
        "    ===============================================",
        "    inode            address_space    nrpages  size       Path",
        "    ffffff805c413178 ffffff805c413340 16220    63.36MB    /app/webview/webview.apk",
        "    ffffff805c4e6128 ffffff805c4e62f0 8768     34.25MB    /priv-app/Settings/Settings.apk",
        "    ffffff8032903848 ffffff8032903a10 8590     33.55MB    /system/framework/framework.jar",
        "    ffffff803d3daaa8 ffffff803d3dac70 4209     16.44MB    /system/framework/services.jar",
        "\n",
        "  Display page cache of inode:",
        "    %s> files -p ffffff805c413178",
        "           INODE        NRPAGES",
        "    ffffff805c413178    16220",
        "    ",
        "          PAGE       PHYSICAL      MAPPING       INDEX CNT FLAGS",
        "    fffffffe00bd2000 6f480000 ffffff805c413340        0  3 10000000020014 uptodate,lru,mappedtodisk",
        "    fffffffe00630ec0 58c3b000 ffffff805c413340        2  3 10000000020014 uptodate,lru,mappedtodisk",
        "    fffffffe00bc6d80 6f1b6000 ffffff805c413340        3  3 10000000020014 uptodate,lru,mappedtodisk",
        "    fffffffe012b4cc0 8ad33000 ffffff805c413340        4  3 10000000020014 uptodate,lru,mappedtodisk",
        "\n",
        "  Display anon pages:",
        "    %s> cache -a",
        "    page:0xfffffffe0007f300  paddr:0x41fcc000",
        "    page:0xfffffffe0007f440  paddr:0x41fd1000",
        "    page:0xfffffffe0007f4c0  paddr:0x41fd3000",
        "\n",
    };
    initialize();
    init_offset();
}

#pragma GCC diagnostic pop
