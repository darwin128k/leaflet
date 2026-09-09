#include "ui_layout.h"
#include "ui_api.h"

static int ClampTrack(int value, const UiGridColumn *column)
{
    if (value < column->minW) {
        value = column->minW;
    }
    if (column->maxW > 0 && value > column->maxW) {
        value = column->maxW;
    }
    return value;
}

static int TrackCanGrow(int width, const UiGridColumn *column)
{
    return column->maxW <= 0 || width < column->maxW;
}

static int TrackCanShrink(int width, const UiGridColumn *column)
{
    return width > column->minW;
}

static void DistributeDelta(UiGrid *grid, const UiGridColumn *columns, int delta)
{
    int guard = 0;
    while (delta != 0 && guard++ < 32) {
        int i;
        int weightSum = 0;
        int changed = 0;
        for (i = 0; i < grid->count; i++) {
            if ((delta > 0 && TrackCanGrow(grid->widths[i], &columns[i]))
                || (delta < 0 && TrackCanShrink(grid->widths[i], &columns[i]))) {
                weightSum += columns[i].basis > 0 ? columns[i].basis : 1;
            }
        }
        if (weightSum <= 0) {
            break;
        }
        for (i = 0; i < grid->count && delta != 0; i++) {
            int before;
            int share;
            int weight = columns[i].basis > 0 ? columns[i].basis : 1;
            if ((delta > 0 && !TrackCanGrow(grid->widths[i], &columns[i]))
                || (delta < 0 && !TrackCanShrink(grid->widths[i], &columns[i]))) {
                continue;
            }
            share = delta * weight / weightSum;
            if (share == 0) {
                share = delta > 0 ? 1 : -1;
            }
            before = grid->widths[i];
            grid->widths[i] = ClampTrack(before + share, &columns[i]);
            delta -= grid->widths[i] - before;
            if (grid->widths[i] != before) {
                changed = 1;
            }
        }
        if (!changed) {
            break;
        }
    }
}

void UiGrid_Init(UiGrid *grid, int x, int y, int w, int h, int gap,
                 const UiGridColumn *columns, int columnCount)
{
    int i;
    int available;
    int used = 0;
    int cursor;
    if (grid == NULL) {
        return;
    }
    grid->bounds.x = x;
    grid->bounds.y = y;
    grid->bounds.w = w > 0 ? w : 0;
    grid->bounds.h = h > 0 ? h : 0;
    grid->gap = gap > 0 ? gap : 0;
    grid->count = 0;
    for (i = 0; i < UI_LAYOUT_MAX_COLUMNS; i++) {
        grid->widths[i] = 0;
        grid->x[i] = x;
    }
    if (columns == NULL || columnCount <= 0) {
        return;
    }
    if (columnCount > UI_LAYOUT_MAX_COLUMNS) {
        columnCount = UI_LAYOUT_MAX_COLUMNS;
    }
    grid->count = columnCount;
    available = grid->bounds.w - grid->gap * (columnCount - 1);
    if (available < 0) {
        available = 0;
    }
    for (i = 0; i < columnCount; i++) {
        int width = available * columns[i].basis / 10000;
        grid->widths[i] = ClampTrack(width, &columns[i]);
        used += grid->widths[i];
    }
    DistributeDelta(grid, columns, available - used);
    used = 0;
    for (i = 0; i < columnCount; i++) {
        used += grid->widths[i];
    }
    /* A window narrower than the sum of all minimums must still stay
     * on-screen. CSS would overflow; Options is more useful if it shrinks. */
    if (used > available && used > 0) {
        int fitted = 0;
        for (i = 0; i < columnCount; i++) {
            int width = grid->widths[i] * available / used;
            if (width < 1 && available > 0) {
                width = 1;
            }
            grid->widths[i] = width;
            fitted += width;
        }
        if (columnCount > 0) {
            grid->widths[columnCount - 1] += available - fitted;
        }
    }

    cursor = x;
    for (i = 0; i < columnCount; i++) {
        grid->x[i] = cursor;
        cursor += grid->widths[i] + grid->gap;
    }
}

UiRect UiGrid_Cell(const UiGrid *grid, int column, int span)
{
    UiRect rect = { 0, 0, 0, 0 };
    int last;
    if (grid == NULL || grid->count <= 0) {
        return rect;
    }
    if (column < 0) {
        column = 0;
    }
    if (column >= grid->count) {
        column = grid->count - 1;
    }
    if (span <= 0) {
        span = 1;
    }
    last = column + span - 1;
    if (last >= grid->count) {
        last = grid->count - 1;
    }
    rect.x = grid->x[column];
    rect.y = grid->bounds.y;
    rect.w = grid->x[last] + grid->widths[last] - rect.x;
    rect.h = grid->bounds.h;
    return rect;
}

void UiGrid_Place(const UiGrid *grid, void *panel, int column, int span,
                  int y, int rowHeight, int panelHeight, UiAlign alignY)
{
    UiRect cell;
    int panelY = y;
    int panelH = panelHeight;
    if (panel == NULL || grid == NULL) {
        return;
    }
    cell = UiGrid_Cell(grid, column, span);
    if (panelH <= 0) {
        panelH = rowHeight;
    }
    if (alignY == UI_ALIGN_CENTER) {
        panelY = y + (rowHeight - panelH) / 2;
    } else if (alignY == UI_ALIGN_END) {
        panelY = y + rowHeight - panelH;
    }
    Ui_Place(panel, cell.x, panelY, cell.w, panelH);
}

int UiTable_Apply(void *parent, const UiGrid *grid, int startY,
                  const UiTableRow *rows, int rowCount)
{
    int row;
    int y = startY;
    if (parent == NULL || grid == NULL || rows == NULL || rowCount <= 0) {
        return y;
    }
    for (row = 0; row < rowCount; row++) {
        int cellIndex;
        const UiTableRow *r = &rows[row];
        for (cellIndex = 0; cellIndex < r->cellCount; cellIndex++) {
            const UiTableCell *cell = &r->cells[cellIndex];
            UiRect columnRect;
            int height;
            int panelY;
            void *panel = cell->panel;
            if (panel == NULL && cell->name != NULL) {
                panel = Ui_Find(parent, cell->name);
            }
            if (panel == NULL) {
                continue;
            }
            columnRect = UiGrid_Cell(grid, cell->column, cell->span);
            height = cell->height > 0 ? cell->height : r->height;
            panelY = y;
            if (cell->alignY == UI_ALIGN_CENTER) {
                panelY += (r->height - height) / 2;
            } else if (cell->alignY == UI_ALIGN_END) {
                panelY += r->height - height;
            }
            panelY += cell->offsetY;
            columnRect.x += cell->insetLeft;
            columnRect.w -= cell->insetLeft + cell->insetRight;
            if (columnRect.w < 1) {
                columnRect.w = 1;
            }
            Ui_Place(panel, columnRect.x, panelY, columnRect.w, height);
        }
        y += r->height + r->gapAfter;
    }
    return y;
}
