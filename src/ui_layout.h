#ifndef LEAFLET_UI_LAYOUT_H
#define LEAFLET_UI_LAYOUT_H

/*
 * Small CSS-grid-like geometry layer for stock VGUI panels.
 *
 * - column basis is expressed in basis points (5000 = 50%)
 * - min/max constraints keep controls usable at narrow/wide resolutions
 * - row tables replace repeated FindChild/SetPos/SetSize chains
 *
 * This module only computes geometry and places existing panel pointers.
 * Control creation remains in ui_api.
 */

#define UI_LAYOUT_MAX_COLUMNS 8

typedef struct UiRect {
    int x;
    int y;
    int w;
    int h;
} UiRect;

typedef struct UiGridColumn {
    int basis; /* 0..10000 */
    int minW;
    int maxW;  /* 0 = unlimited */
} UiGridColumn;

typedef struct UiGrid {
    UiRect bounds;
    int gap;
    int count;
    int widths[UI_LAYOUT_MAX_COLUMNS];
    int x[UI_LAYOUT_MAX_COLUMNS];
} UiGrid;

typedef enum UiAlign {
    UI_ALIGN_FILL = 0,
    UI_ALIGN_START,
    UI_ALIGN_CENTER,
    UI_ALIGN_END
} UiAlign;

typedef struct UiTableCell {
    const char *name; /* looked up under parent when panel is NULL */
    void *panel;
    int column;
    int span;
    int height;       /* 0 = fill row */
    UiAlign alignY;
    int insetLeft;
    int insetRight;
    int offsetY;
} UiTableCell;

typedef struct UiTableRow {
    const UiTableCell *cells;
    int cellCount;
    int height;
    int gapAfter;
} UiTableRow;

void UiGrid_Init(UiGrid *grid, int x, int y, int w, int h, int gap,
                 const UiGridColumn *columns, int columnCount);
UiRect UiGrid_Cell(const UiGrid *grid, int column, int span);
void UiGrid_Place(const UiGrid *grid, void *panel, int column, int span,
                  int y, int rowHeight, int panelHeight, UiAlign alignY);

/* Returns the first y after the final row. Missing names are harmless. */
int UiTable_Apply(void *parent, const UiGrid *grid, int startY,
                  const UiTableRow *rows, int rowCount);

#endif
