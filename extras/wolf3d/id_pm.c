// SPDX-License-Identifier: GPL-2.0-only
// From Wolf4SDL (see PROVENANCE), changed for the X11 port:
//   VSWAP pages are read from the file when asked for, into a small cache,
//   instead of all at startup.
// ID_PM.C

#include "wl_def.h"

#ifdef XWOLF
//
// XWOLF: the original reads all of VSWAP into memory (742 KB for the
// shareware data), more than the board can spare. Pages are read from the
// file when asked for instead, into a small cache of the most recently used
// ones. A pointer from PM_GetPage stays good until PM_CACHE_PAGES other
// pages have been read, which is longer than any caller holds one: they use
// a page at once (the X server keeps its own copy of every wall texture).
//
#define PM_CACHE_PAGES 24

word ChunksInFile;
word PMSpriteStart;
word PMSoundStart;
bool PMSoundInfoPagePadded = false;
word *pageLengths;
byte **PMPages;                         // unused here

static FILE     *pmfile;
static uint32_t *pmoffs, *pmsize;
static struct { int page; uint32_t used; byte *data; } pmcache[PM_CACHE_PAGES];
static uint32_t pmclock;
static uint32_t pmloads;

void PM_Startup (void)
{
    char fname[13];
    long filesize;
    int i;

    snprintf (fname,sizeof(fname),"vswap.%s",extension);
    pmfile = fopen(fname,"rb");
    if (!pmfile)
        CA_CannotOpen(fname);
    fread (&ChunksInFile,sizeof(ChunksInFile),1,pmfile);
    fread (&PMSpriteStart,sizeof(PMSpriteStart),1,pmfile);
    fread (&PMSoundStart,sizeof(PMSoundStart),1,pmfile);
    pmoffs = SafeMalloc((ChunksInFile + 1) * sizeof(*pmoffs));
    pmsize = SafeMalloc(ChunksInFile * sizeof(*pmsize));
    pageLengths = SafeMalloc(ChunksInFile * sizeof(*pageLengths));
    fread (pmoffs,sizeof(*pmoffs),ChunksInFile,pmfile);
    fread (pageLengths,sizeof(*pageLengths),ChunksInFile,pmfile);
    fseek (pmfile,0,SEEK_END);
    filesize = ftell(pmfile);
    for (i = 0; i < ChunksInFile; i++)
    {
        uint32_t next = filesize;
        int j;

        // a page runs to the next page that has data, or its stated length
        for (j = i + 1; j < ChunksInFile && !pmoffs[j]; j++)
            ;
        if (j < ChunksInFile)
            next = pmoffs[j];
        if (!pmoffs[i])
            pmsize[i] = 0;          // sparse page
        else if (i + 1 < ChunksInFile && !pmoffs[i + 1])
            pmsize[i] = pageLengths[i];
        else
            pmsize[i] = next - pmoffs[i];
        if (pmoffs[i] && (pmoffs[i] >= (uint32_t)filesize || pmsize[i] > (uint32_t)filesize - pmoffs[i]))
            Quit ("PM_Startup: Illegal page offset for page %i: %u (filesize: %ld)",i,pmoffs[i],filesize);
    }
    for (i = 0; i < PM_CACHE_PAGES; i++)
        pmcache[i].page = -1;
}

void PM_Shutdown (void)
{
    int i;

    for (i = 0; i < PM_CACHE_PAGES; i++)
    {
        free (pmcache[i].data);
        pmcache[i].data = NULL;
        pmcache[i].page = -1;
    }
    free (pmoffs);
    free (pmsize);
    free (pageLengths);
    pmoffs = pmsize = NULL;
    pageLengths = NULL;
    if (pmfile)
        fclose (pmfile);
    pmfile = NULL;
}

uint32_t PM_GetPageSize (int page)
{
    if (page < 0 || page >= ChunksInFile)
        Quit ("PM_GetPageSize: Invalid page request: %i",page);
    return pmsize[page];
}

byte *PM_GetPage (int page)
{
    int i, slot = 0;

    if (page < 0 || page >= ChunksInFile)
        Quit ("PM_GetPage: Invalid page request: %i",page);
    for (i = 0; i < PM_CACHE_PAGES; i++)
    {
        if (pmcache[i].page == page)
        {
            pmcache[i].used = ++pmclock;
            return pmcache[i].data;
        }
        if (pmcache[i].used < pmcache[slot].used)
            slot = i;
    }
    // every slot holds PMPageSize, or more for the odd big page
    if (pmcache[slot].data == NULL || pmsize[page] > PMPageSize)
    {
        free (pmcache[slot].data);
        pmcache[slot].data = SafeMalloc(pmsize[page] > PMPageSize ? pmsize[page] : PMPageSize);
    }
    if (pmsize[page])
    {
        fseek (pmfile,pmoffs[page],SEEK_SET);
        if (fread (pmcache[slot].data,1,pmsize[page],pmfile) != pmsize[page])
            Quit ("PM_GetPage: Can't read page %i",page);
    }
    pmcache[slot].page = page;
    pmcache[slot].used = ++pmclock;
    pmloads++;
    return pmcache[slot].data;
}

byte *PM_GetPageEnd (void)
{
    Quit ("PM_GetPageEnd: not available in this build");
    return NULL;
}

#else

word ChunksInFile;
word PMSpriteStart;
word PMSoundStart;

bool PMSoundInfoPagePadded = false;

word *pageLengths;

byte *PMPageData;
byte **PMPages;


/*
==================
=
= PM_Startup
=
==================
*/

void PM_Startup (void)
{
    int      i;
    int      padding;
    byte     *page;
    uint32_t *pageOffsets;
    uint32_t pagesize;
    int32_t  filesize,datasize;
    FILE     *file;
    char     fname[13];

    snprintf (fname,sizeof(fname),"vswap.%s",extension);

    file = fopen(fname,"rb");

    if (!file)
        CA_CannotOpen(fname);

    //
    // read in header variables
    //
    fread (&ChunksInFile,sizeof(ChunksInFile),1,file);
    fread (&PMSpriteStart,sizeof(PMSpriteStart),1,file);
    fread (&PMSoundStart,sizeof(PMSoundStart),1,file);

    //
    // read in the chunk offsets
    //
    pageOffsets = SafeMalloc((ChunksInFile + 1) * sizeof(*pageOffsets));

    fread (pageOffsets,sizeof(*pageOffsets),ChunksInFile,file);

    //
    // read in the chunk lengths
    //
    pageLengths = SafeMalloc(ChunksInFile * sizeof(*pageLengths));

    fread (pageLengths,sizeof(*pageLengths),ChunksInFile,file);

    fseek (file,0,SEEK_END);
    filesize = ftell(file);
    datasize = filesize - pageOffsets[0];

    if (datasize < 0)
        Quit ("PM_Startup: The page file \"%s\" is too large!",fname);

    pageOffsets[ChunksInFile] = filesize;

    //
    // check that all chunk offsets are valid
    //
    for (i = 0; i < ChunksInFile; i++)
    {
        if (!pageOffsets[i])
            continue;           // sparse page

        if (pageOffsets[i] < pageOffsets[0] || pageOffsets[i] >= (size_t)filesize)
            Quit ("PM_Startup: Illegal page offset for page %i: %u (filesize: %u)",i,pageOffsets[i],filesize);
    }

    //
    // calculate total amount of padding needed for sprites and sound info page
    //
    padding = 0;

    for (i = PMSpriteStart; i < PMSoundStart; i++)
    {
        if (!pageOffsets[i])
            continue;           // sparse page

        if (((pageOffsets[i] - pageOffsets[0]) + padding) & 1)
            padding++;
    }

    if (((pageOffsets[ChunksInFile - 1] - pageOffsets[0]) + padding) & 1)
        padding++;

    //
    // allocate enough memory to hold the whole page file
    //
    PMPageData = SafeMalloc(datasize + padding);

    //
    // [ChunksInFile + 1] pointers to page starts
    // the last pointer points one byte after the last page
    //
    PMPages = SafeMalloc((ChunksInFile + 1) * sizeof(*PMPages));

    //
    // load pages and initialize PMPages pointers
    //
    page = &PMPageData[0];

    for (i = 0; i < ChunksInFile; i++)
    {
        if ((i >= PMSpriteStart && i < PMSoundStart) || i == ChunksInFile - 1)
        {
            //
            // pad with zeros to make it 2-byte aligned
            //
            if ((page - &PMPageData[0]) & 1)
            {
                *page++ = 0;

                if (i == ChunksInFile - 1)
                    PMSoundInfoPagePadded = true;
            }
        }

        PMPages[i] = page;

        if (!pageOffsets[i])
            continue;               // sparse page

        //
        // use specified page length when next page is sparse
        // otherwise, calculate size from the offset difference between this and the next page
        //
        if (!pageOffsets[i + 1])
            pagesize = pageLengths[i];
        else
            pagesize = pageOffsets[i + 1] - pageOffsets[i];

        fseek (file,pageOffsets[i],SEEK_SET);
        fread (page,sizeof(*page),pagesize,file);

        page += pagesize;
    }

    //
    // last page points after page buffer
    //
    PMPages[ChunksInFile] = page;

    free (pageOffsets);

    fclose (file);
}


/*
==================
=
= PM_Shutdown
=
==================
*/

void PM_Shutdown (void)
{
    free (pageLengths);
    free (PMPages);
    free (PMPageData);

    pageLengths = NULL;
    PMPages = NULL;
    PMPageData = NULL;
}


/*
==================
=
= PM_GetPageSize
=
==================
*/

uint32_t PM_GetPageSize (int page)
{
    if (page < 0 || page >= ChunksInFile)
        Quit ("PM_GetPageSize: Invalid page request: %i",page);

    return (uint32_t)(PMPages[page + 1] - PMPages[page]);
}


/*
==================
=
= PM_GetPage
=
= Returns the address of the page
=
==================
*/

byte *PM_GetPage (int page)
{
    if (page < 0 || page >= ChunksInFile)
        Quit ("PM_GetPage: Invalid page request: %i",page);

    return PMPages[page];
}


/*
==================
=
= PM_GetPageEnd
=
= Returns the address of the last page
=
==================
*/

byte *PM_GetPageEnd (void)
{
    return PMPages[ChunksInFile];
}

#endif // XWOLF
