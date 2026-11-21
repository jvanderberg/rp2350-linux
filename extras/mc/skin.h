/*
 * skin.h - every colour the program uses, in one place.
 *
 * The attribute names and values follow mc's "default" skin
 * (/usr/share/mc/skins/default.ini), so the result looks like stock mc.
 * To re-theme, edit this file only.
 */
#ifndef SKIN_H
#define SKIN_H

#include "screen.h"

/*
 * The xterm-256 colour index used for each of the 16 logical colours.
 * The values approximate the VGA text palette mc was designed on. In
 * particular "blue" is the deep #0000af of a PC console rather than the
 * lighter blue most terminal themes use for ANSI colour 4.
 * Run with MC_16COLORS=1 to use the terminal's own 16 colours instead.
 */
#define SKIN_PALETTE {                                           \
    16,  /* black          #000000 */                            \
    124, /* red            #af0000 */                            \
    34,  /* green          #00af00 */                            \
    130, /* brown          #af5f00 */                            \
    19,  /* blue           #0000af  (panel background) */        \
    127, /* magenta        #af00af */                            \
    37,  /* cyan           #00afaf */                            \
    248, /* lightgray      #a8a8a8 */                            \
    240, /* gray           #585858 */                            \
    203, /* brightred      #ff5f5f */                            \
    83,  /* brightgreen    #5fff5f */                            \
    227, /* yellow         #ffff5f */                            \
    63,  /* brightblue     #5f5fff */                            \
    207, /* brightmagenta  #ff5fff */                            \
    87,  /* brightcyan     #5fffff */                            \
    231, /* white          #ffffff */                            \
}

/* Panels */
#define A_PANEL        ATTR(C_LIGHTGRAY, C_BLUE)   /* frame, plain files   */
#define A_HEADER       ATTR(C_YELLOW, C_BLUE)      /* column titles        */
#define A_CURSOR       ATTR(C_BLACK, C_CYAN)       /* selection bar        */
#define A_MARKED       ATTR(C_YELLOW, C_BLUE)      /* tagged files         */
#define A_MARKED_CUR   ATTR(C_YELLOW, C_CYAN)      /* tagged + under bar   */
#define A_TITLE_ACTIVE ATTR(C_BLACK, C_LIGHTGRAY)  /* path of active panel */

/* Foreground colours for file types (mc's filehighlight.ini); the
 * background is always the panel blue. */
#define FG_DIR         C_WHITE
#define FG_EXEC        C_BRIGHTGREEN
#define FG_LINK        C_LIGHTGRAY
#define FG_STALE       C_BRIGHTRED
#define FG_DEVICE      C_BRIGHTMAGENTA
#define FG_SPECIAL     C_BLACK
#define FG_CORE        C_RED
#define FG_TEMP        C_GRAY
#define FG_ARCHIVE     C_BRIGHTMAGENTA
#define FG_DOC         C_BROWN
#define FG_SOURCE      C_CYAN
#define FG_MEDIA       C_GREEN
#define FG_GRAPH       C_BRIGHTCYAN
#define FG_DATABASE    C_BRIGHTRED

/* Top menu bar and pull-down menus */
#define A_MENUBAR      ATTR(C_BLACK, C_CYAN)       /* bar while inactive   */
#define A_MENU         ATTR(C_WHITE, C_CYAN)
#define A_MENU_HOT     ATTR(C_YELLOW, C_CYAN)
#define A_MENU_SEL     ATTR(C_WHITE, C_BLACK)
#define A_MENU_HOTSEL  ATTR(C_YELLOW, C_BLACK)

/* Bottom of the screen */
#define A_SHELL        ATTR(C_DEFAULT, C_DEFAULT)  /* hint + command line  */
#define A_KEY_NUM      ATTR(C_WHITE, C_BLACK)      /* "1" .. "10"          */
#define A_KEY_LABEL    ATTR(C_BLACK, C_CYAN)       /* "Help", "Menu", ...  */

/* Dialogs: normal (grey) and error (red) colour sets */
#define A_DLG          ATTR(C_BLACK, C_LIGHTGRAY)
#define A_DLG_HOT      ATTR(C_BLUE, C_LIGHTGRAY)
#define A_DLG_FOCUS    ATTR(C_BLACK, C_CYAN)
#define A_DLG_HOTFOCUS ATTR(C_BLUE, C_CYAN)
#define A_DLG_TITLE    ATTR(C_BLUE, C_LIGHTGRAY)

#define A_ERR          ATTR(C_WHITE, C_RED)
#define A_ERR_HOT      ATTR(C_YELLOW, C_RED)
#define A_ERR_FOCUS    ATTR(C_BLACK, C_LIGHTGRAY)
#define A_ERR_HOTFOCUS ATTR(C_YELLOW, C_LIGHTGRAY)
#define A_ERR_TITLE    ATTR(C_YELLOW, C_RED)

#define A_INPUT        ATTR(C_BLACK, C_CYAN)       /* edited input field   */
#define A_INPUT_UNCH   ATTR(C_GRAY, C_CYAN)        /* pre-filled, untouched */
#define A_GAUGE        ATTR(C_WHITE, C_BLACK)      /* progress bar fill    */

#define A_SHADOW       ATTR(C_GRAY, C_BLACK)       /* dialog drop shadow   */

/* Viewer (F3). The markdown colours are this program's own; mc's viewer
 * has no markdown mode. */
#define A_VIEW         ATTR(C_LIGHTGRAY, C_BLUE)   /* text                 */
#define A_VIEW_STATUS  ATTR(C_BLACK, C_CYAN)       /* top status line      */
#define A_VIEW_FOUND   ATTR(C_YELLOW, C_CYAN)      /* search match         */
#define A_VIEW_DIM     ATTR(C_GRAY, C_BLUE)        /* hex offsets          */
#define A_MD_H1        ATTR(C_WHITE, C_CYAN)       /* # heading: a band    */
#define A_MD_H2        ATTR(C_YELLOW, C_BLUE)      /* ## heading           */
#define A_MD_H3        ATTR(C_WHITE, C_BLUE)       /* ### and deeper       */
#define A_MD_BOLD      ATTR(C_WHITE, C_BLUE)       /* **bold**             */
#define A_MD_ITALIC    ATTR(C_CYAN, C_BLUE)        /* *italic*             */
#define A_MD_CODE      ATTR(C_BRIGHTGREEN, C_BLUE) /* `code`, code blocks  */
#define A_MD_LINK      ATTR(C_BRIGHTCYAN, C_BLUE)  /* [link text]          */
#define A_MD_MARK      ATTR(C_YELLOW, C_BLUE)      /* bullets, numbers     */
#define A_MD_RULE      ATTR(C_GRAY, C_BLUE)        /* rules, table lines, urls */
#define A_MD_QUOTE     ATTR(C_CYAN, C_BLUE)        /* > quoted text        */

#endif /* SKIN_H */
