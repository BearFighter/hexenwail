/* inv_ui.c -- Diablo-style inventory overlay (docs/items-inventory-spec.md).
 *
 * Real-time overlay: the menu layer keeps key_dest == key_menu so gameplay
 * input is swallowed, but the server is deliberately not paused.  This file
 * owns the layout, hit-testing and drawing behind m_inventory.
 *
 * Layout (CANVAS_MENU units, 320x200):
 *   - title strip at the top,
 *   - page-0 equipment slots 0-12 across one row (10-12 are rune sockets),
 *   - backpack grid of the 60 cells (page 0 slots 16-31, pages 1-2 slots
 *     0-31), ten columns wide and six rows tall.
 */

#include "quakedef.h"
#include "inv_ui.h"
#include "draw.h"
#include "menu.h"
#include "client.h"

/* Screen geometry in CANVAS_MENU units (320x200). */
#define	INV_CANVAS_W		320
#define	INV_CANVAS_H		200
#define	INV_TITLE_X		132
#define	INV_TITLE_Y		4
#define	INV_TIP_X		8
#define	INV_TIP_Y		42
#define	INV_EQUIP_X		4
#define	INV_EQUIP_Y		20
#define	INV_EQUIP_PITCH	24
#define	INV_EQUIP_ICON	20
#define	INV_GRID_X		40
#define	INV_GRID_Y		56
#define	INV_GRID_PITCH	24
#define	INV_GRID_COLS	10
#define	INV_GRID_ROWS	6
#define	INV_CELL_ICON	20
#define	INV_BACKPACK_CELLS	(INV_GRID_COLS * INV_GRID_ROWS)
#define	INV_OCCUPIED_ID		255	/* covered cell of a multi-cell item */

typedef struct inv_rect_t
{
	int		x, y, w, h;
} inv_rect_t;

void INV_ScreenInit (inv_screen_t *s, client_state_t *client)
{
	s->client = client;
	s->page = 0;
	s->slot = 16;
	s->hover_page = -1;
	s->hover_slot = -1;
	s->hover_valid = 0;
}

void INV_ScreenShutdown (inv_screen_t *s)
{
	s->client = 0;
	s->hover_valid = 0;
}

/* Fill helper that works on all three renderer paths.  The GL/WebGL builds get
 * an alpha-blended panel; the 8-bit software build falls back to palette 0. */
static void Inv_FillRect (int x, int y, int w, int h,
			  float r, float g, float b, float a)
{
#if defined(GLQUAKE) || defined(WEBQUAKE)
	Draw_FillAlpha (x, y, w, h, r, g, b, a);
#else
	Draw_Fill (x, y, w, h, 0);
#endif
}

/* Read an item out of the client's extended-inventory pages: page, slot ->
 * item id + count.  Returns the item id; 0 means empty. */
static float Inv_Find (client_state_t *client, int page, int slot, float *count)
{
	ex_inventory_page_t *p;
	if (!client || page < 0 || page >= MAX_INVENTORY_EX_PAGES)
	{
		*count = 0;
		return 0;
	}
	p = &client->ex_inventory[page];
	if (slot < 0 || slot >= INV_SCREEN_SLOTS || !p->item_id[slot])
	{
		*count = 0;
		return 0;
	}
	*count = p->item_cnt[slot];
	return p->item_id[slot];
}

/* Item registry strings are "gfx/artiNN.lmp" for a 1x1 item and
 * "gfx/artiNN.lmp#WxH" for one with a larger footprint.  The suffix rides
 * along the existing ex-item registration, so no protocol field is added. */
static void Inv_ParseIconString (const char *src, char *path, int size,
				 int *w, int *h)
{
	int n = 0;
	const char *p;
	int tw = 0, th = 0;

	*w = 1;
	*h = 1;
	if (!src)
	{
		path[0] = 0;
		return;
	}
	while (src[n] && src[n] != '#' && n + 1 < size)
	{
		path[n] = src[n];
		n++;
	}
	path[n] = 0;
	if (src[n] != '#')
		return;

	p = src + n + 1;
	while (*p >= '0' && *p <= '9')
	{
		tw = tw * 10 + (*p - '0');
		p++;
	}
	if (*p != 'x')
		return;
	p++;
	while (*p >= '0' && *p <= '9')
	{
		th = th * 10 + (*p - '0');
		p++;
	}
	if (*p || tw < 1 || th < 1)
		return;

	*w = tw > INV_GRID_COLS ? INV_GRID_COLS : tw;
	*h = th > INV_GRID_ROWS ? INV_GRID_ROWS : th;
}

/* Item id -> icon path and footprint through the client's ex item list. */
static void Inv_ItemInfo (client_state_t *client, float item_id,
			  char *path, int size, int *w, int *h)
{
	int i;

	path[0] = 0;
	*w = 1;
	*h = 1;
	if (!client)
		return;
	for (i = 0; i < client->num_ex_items; i++)
	{
		if (client->ex_items[i].id == (int)item_id)
		{
			Inv_ParseIconString (client->ex_items[i].icon,
					     path, size, w, h);
			return;
		}
	}
}

/* Draw one cell of a multi-cell item.  The source icon is split into a grid
 * of span_w x span_h slices and this cell draws its own slice.  Every cell of
 * the footprint therefore draws through the renderer's normal Draw_SubPic
 * path: no hand-built qpic_t, no stretched quad that can disturb later 2D
 * draws, and no second copy of the stack count on covered cells. */
static void Inv_DrawItemSlice (client_state_t *client, inv_rect_t r,
			       float item_id, float count,
			       int span_w, int span_h, int dx, int dy,
			       int show_count)
{
	char	path[MAX_QPATH];
	int	w, h;
	int	sx, sy, sw, sh;
	qpic_t	*pic;

	Inv_ItemInfo (client, item_id, path, sizeof(path), &w, &h);
	if (!path[0])
		return;
	pic = Draw_CachePic (path);
	if (!pic || pic->width <= 0 || pic->height <= 0)
		return;

	if (span_w <= 1 && span_h <= 1)
	{
		Draw_Pic (r.x, r.y, pic);
	}
	else
	{
		if (dx < 0 || dx >= span_w || dy < 0 || dy >= span_h)
			return;
		sx = dx * pic->width / span_w;
		sy = dy * pic->height / span_h;
		sw = (pic->width - sx) / (span_w - dx);
		sh = (pic->height - sy) / (span_h - dy);
		if (sw < 1) sw = 1;
		if (sh < 1) sh = 1;
		Draw_SubPic (r.x, r.y, pic, sx, sy, sw, sh);
	}

	if (show_count && count > 1)
	{
		char buf[16];
		int val = (int)count;
		int len = 0;
		char tmp[16];
		int i;
		if (val <= 0)
			tmp[len++] = '0';
		while (val > 0)
		{
			tmp[len++] = (char)('0' + (val % 10));
			val /= 10;
		}
		for (i = 0; i < len; i++)
			buf[i] = tmp[len - 1 - i];
		buf[len] = 0;
		M_Print (r.x + r.w - 8, r.y + r.h - 8, buf);
	}
}

/* Map a backpack display index (0..59) to its (page, slot). */
static void Inv_CellForLocal (int li, int *page, int *slot)
{
	if (li < 16)
	{
		*page = 0;
		*slot = 16 + li;
		return;
	}
	{
		int l2 = li - 16;
		*page = 1 + (l2 / 32);
		*slot = l2 % 32;
	}
}

/* Inverse map, used to resolve a covered cell back to its item anchor. */
static int Inv_LocalForCell (int page, int slot)
{
	if (page == 0 && slot >= 16 && slot < 32)
		return slot - 16;
	if (page == 1 && slot >= 0 && slot < 32)
		return slot;
	if (page == 2 && slot >= 0 && slot < 32)
		return slot + 16;
	return -1;
}

/* Footprint comes from the item registry, not from the stack count.  Counts
 * stay honest (and savegames stay readable) while the icon string carries the
 * size the generator authored in items.json. */
static void Inv_SpanForAnchor (client_state_t *client, int anchor,
			       int *w, int *h)
{
	int page, slot, col, row;
	float count;
	float id;
	char path[MAX_QPATH];

	*w = 1;
	*h = 1;
	if (anchor < 0 || anchor >= INV_BACKPACK_CELLS)
		return;

	Inv_CellForLocal (anchor, &page, &slot);
	id = Inv_Find (client, page, slot, &count);
	if (id <= 0 || (int)id == INV_OCCUPIED_ID)
		return;

	Inv_ItemInfo (client, id, path, sizeof(path), w, h);
	col = anchor % INV_GRID_COLS;
	row = anchor / INV_GRID_COLS;
	if (col + *w > INV_GRID_COLS)
		*w = INV_GRID_COLS - col;
	if (row + *h > INV_GRID_ROWS)
		*h = INV_GRID_ROWS - row;
	if (*w < 1) *w = 1;
	if (*h < 1) *h = 1;
}

static int Inv_SpanContains (int anchor, int w, int h, int li)
{
	int arow, acol, row, col;

	if (anchor < 0 || anchor >= INV_BACKPACK_CELLS ||
	    li < 0 || li >= INV_BACKPACK_CELLS)
		return 0;
	arow = anchor / INV_GRID_COLS;
	acol = anchor % INV_GRID_COLS;
	row = li / INV_GRID_COLS;
	col = li % INV_GRID_COLS;
	return row >= arow && row < arow + h &&
	       col >= acol && col < acol + w;
}

/* Resolve a covered cell to its anchor.  The covered-cell marker is the fast
 * path; the registry footprint is the fallback if a marker was not delivered. */
static int Inv_AnchorForCell (client_state_t *client, int li)
{
	int i, page, slot, w, h;
	float count;
	float id;

	if (li < 0 || li >= INV_BACKPACK_CELLS)
		return -1;

	Inv_CellForLocal (li, &page, &slot);
	id = Inv_Find (client, page, slot, &count);
	if ((int)id == INV_OCCUPIED_ID)
	{
		i = (int)count;
		if (i >= 0 && i < INV_BACKPACK_CELLS)
		{
			Inv_SpanForAnchor (client, i, &w, &h);
			if (Inv_SpanContains (i, w, h, li))
				return i;
		}
	}

	for (i = 0; i < INV_BACKPACK_CELLS; i++)
	{
		Inv_CellForLocal (i, &page, &slot);
		id = Inv_Find (client, page, slot, &count);
		if (id <= 0 || (int)id == INV_OCCUPIED_ID)
			continue;
		Inv_SpanForAnchor (client, i, &w, &h);
		if (Inv_SpanContains (i, w, h, li))
			return i;
	}
	return -1;
}

/* Selection and hover both resolve through the footprint, so clicking any
 * cell of a 3x2 item highlights the whole item, not one square of it. */
static int Inv_SelectionAnchor (const inv_screen_t *s)
{
	int li = Inv_LocalForCell (s->page, s->slot);
	return li >= 0 ? Inv_AnchorForCell (s->client, li) : -1;
}

static int Inv_HoverAnchor (const inv_screen_t *s)
{
	int li;
	if (!s->hover_valid)
		return -1;
	li = Inv_LocalForCell (s->hover_page, s->hover_slot);
	return li >= 0 ? Inv_AnchorForCell (s->client, li) : -1;
}

static inv_rect_t Inv_EquipmentRect (int slot)
{
	inv_rect_t r;
	r.x = INV_EQUIP_X + slot * INV_EQUIP_PITCH;
	r.y = INV_EQUIP_Y;
	r.w = INV_EQUIP_ICON;
	r.h = INV_EQUIP_ICON;
	return r;
}

static inv_rect_t Inv_BackpackRect (int li)
{
	inv_rect_t r;
	r.x = INV_GRID_X + (li % INV_GRID_COLS) * INV_GRID_PITCH;
	r.y = INV_GRID_Y + (li / INV_GRID_COLS) * INV_GRID_PITCH;
	r.w = INV_CELL_ICON;
	r.h = INV_CELL_ICON;
	return r;
}

static int Inv_ScreenIndexFor (const inv_screen_t *s)
{
	int li;
	for (li = 0; li < INV_BACKPACK_CELLS; li++)
	{
		int page, slot;
		Inv_CellForLocal (li, &page, &slot);
		if (s->page == page && s->slot == slot)
			return li;
	}
	return 0;
}

static void Inv_DrawCellFrame (inv_rect_t r, int selected, int hovered)
{
	/* Cell body. */
	Inv_FillRect (r.x - 2, r.y - 2, r.w + 4, r.h + 4,
		      0.03f, 0.04f, 0.03f, 0.74f);

	/* Hairline border; hover brightens, selection turns blue. */
	{
		float cr = hovered ? 0.95f : (selected ? 0.35f : 0.55f);
		float cg = hovered ? 0.85f : (selected ? 0.70f : 0.45f);
		float cb = hovered ? 0.35f : (selected ? 1.00f : 0.18f);
		float ca = hovered || selected ? 1.0f : 0.80f;
		Inv_FillRect (r.x - 2, r.y - 2, r.w + 4, 1, cr, cg, cb, ca);
		Inv_FillRect (r.x - 2, r.y + r.h + 1, r.w + 4, 1, cr, cg, cb, ca);
		Inv_FillRect (r.x - 2, r.y - 2, 1, r.h + 4, cr, cg, cb, ca);
		Inv_FillRect (r.x + r.w + 1, r.y - 2, 1, r.h + 4, cr, cg, cb, ca);
	}
}

/* Draw one cell of the grid.  Frames always draw, including cells covered by
 * a multi-cell item, so the rows stay intact.  Selection/hover are resolved
 * to the item's anchor first, making the whole footprint highlight. */
static void Inv_DrawCell (const inv_screen_t *s, inv_rect_t r,
			  int page, int slot)
{
	int li = Inv_LocalForCell (page, slot);
	int anchor = -1;
	int sel_anchor = Inv_SelectionAnchor (s);
	int hov_anchor = Inv_HoverAnchor (s);
	int selected, hovered;

	if (li >= 0)
		anchor = Inv_AnchorForCell (s->client, li);

	if (li >= 0)
	{
		if (anchor >= 0)
		{
			selected = (sel_anchor == anchor);
			hovered = (hov_anchor == anchor);
		}
		else
		{
			selected = (sel_anchor < 0 && s->page == page &&
				    s->slot == slot);
			hovered = (hov_anchor < 0 && s->hover_valid &&
				   s->hover_page == page && s->hover_slot == slot);
		}
	}
	else
	{
		selected = (s->page == page && s->slot == slot);
		hovered = (s->hover_valid && s->hover_page == page &&
			   s->hover_slot == slot);
	}

	Inv_DrawCellFrame (r, selected, hovered);
}

/* Draw an item from its anchor cell across every cell of its footprint.
 * Covered-cell markers are only needed for hit-testing; art never depends on
 * them arriving, so a missing marker cannot leave half an icon missing. */
static void Inv_DrawItemSpan (const inv_screen_t *s, inv_rect_t r,
			      int page, int slot)
{
	float count = 0;
	float id = Inv_Find (s->client, page, slot, &count);
	int li = Inv_LocalForCell (page, slot);
	int anchor, w = 1, h = 1;
	int apage, aslot, aid;
	float acount = 0;
	int acol, arow, dy, dx, nli;

	if (id <= 0)
		return;

	if (li < 0)
	{
		/* Equipment row: one full icon, no footprint. */
		Inv_DrawItemSlice (s->client, r, id, count, 1, 1, 0, 0, 1);
		return;
	}

	anchor = Inv_AnchorForCell (s->client, li);
	if (anchor < 0 || anchor != li)
		return;

	Inv_CellForLocal (anchor, &apage, &aslot);
	aid = Inv_Find (s->client, apage, aslot, &acount);
	if (aid <= 0 || (int)aid == INV_OCCUPIED_ID)
		return;

	Inv_SpanForAnchor (s->client, anchor, &w, &h);
	acol = anchor % INV_GRID_COLS;
	arow = anchor / INV_GRID_COLS;

	dy = 0;
	while (dy < h)
	{
		dx = 0;
		while (dx < w)
		{
			nli = (arow + dy) * INV_GRID_COLS + acol + dx;
			if (nli >= 0 && nli < INV_BACKPACK_CELLS)
				Inv_DrawItemSlice (s->client,
						   Inv_BackpackRect (nli),
						   aid, acount, w, h, dx, dy,
						   dx == 0 && dy == 0);
			dx++;
		}
		dy++;
	}
}

void INV_ScreenDraw (const inv_screen_t *s)
{
	int slot, li;

	/* Dark panel over the live world: readable, but the game stays visible. */
	Inv_FillRect (0, 0, INV_CANVAS_W, INV_CANVAS_H,
		      0.02f, 0.025f, 0.02f, 0.72f);
	Inv_FillRect (0, 0, INV_CANVAS_W, 15,
		      0.16f, 0.13f, 0.05f, 0.92f);

	M_Print (INV_TITLE_X, INV_TITLE_Y, "Inventory");
	M_Print (INV_TIP_X, INV_TIP_Y,
		 "LMB sel RMB use E equip D drop TAB exit");

	/* Pass 1: the grid, so every row stays visible under large items. */
	for (slot = 0; slot < INV_SCREEN_EQUIP_SLOTS; slot++)
		Inv_DrawCell (s, Inv_EquipmentRect (slot), 0, slot);
	for (li = 0; li < INV_BACKPACK_CELLS; li++)
	{
		int page, slot2;
		Inv_CellForLocal (li, &page, &slot2);
		Inv_DrawCell (s, Inv_BackpackRect (li), page, slot2);
	}

	/* Pass 2: item art over its full footprint, drawn from the anchor. */
	for (slot = 0; slot < INV_SCREEN_EQUIP_SLOTS; slot++)
		Inv_DrawItemSpan (s, Inv_EquipmentRect (slot), 0, slot);
	for (li = 0; li < INV_BACKPACK_CELLS; li++)
	{
		int page, slot2;
		Inv_CellForLocal (li, &page, &slot2);
		Inv_DrawItemSpan (s, Inv_BackpackRect (li), page, slot2);
	}
}

/* Hit-test both the equipment row and the backpack grid, including empty
 * cells so clicking a cell always moves the selection. */
static int Inv_CellAt (const inv_screen_t *s, int vx, int vy,
		       int *page, int *slot)
{
	if (vy >= INV_EQUIP_Y && vy < INV_EQUIP_Y + INV_EQUIP_PITCH &&
	    vx >= INV_EQUIP_X && vx < INV_EQUIP_X + INV_SCREEN_EQUIP_SLOTS * INV_EQUIP_PITCH)
	{
		*page = 0;
		*slot = (vx - INV_EQUIP_X) / INV_EQUIP_PITCH;
		return 1;
	}

	if (vx >= INV_GRID_X && vy >= INV_GRID_Y)
	{
		int col = (vx - INV_GRID_X) / INV_GRID_PITCH;
		int row = (vy - INV_GRID_Y) / INV_GRID_PITCH;
		if (col >= 0 && col < INV_GRID_COLS && row >= 0 && row < INV_GRID_ROWS)
		{
			Inv_CellForLocal (row * INV_GRID_COLS + col, page, slot);
			return 1;
		}
	}
	return 0;
}

int INV_ScreenClick (inv_screen_t *s, int vx, int vy)
{
	int page, slot, li, anchor;
	if (!Inv_CellAt (s, vx, vy, &page, &slot))
		return 0;

	/* Clicking any cell of a multi-cell item selects the item's anchor. */
	li = Inv_LocalForCell (page, slot);
	anchor = Inv_AnchorForCell (s->client, li);
	if (anchor >= 0)
	{
		Inv_CellForLocal (anchor, &page, &slot);
	}

	s->page = page;
	s->slot = slot;
	s->hover_page = page;
	s->hover_slot = slot;
	s->hover_valid = 1;
	return 1;
}

void INV_ScreenHover (inv_screen_t *s, int vx, int vy)
{
	int page, slot, li, anchor;
	if (Inv_CellAt (s, vx, vy, &page, &slot))
	{
		li = Inv_LocalForCell (page, slot);
		anchor = Inv_AnchorForCell (s->client, li);
		if (anchor >= 0)
			Inv_CellForLocal (anchor, &page, &slot);
		s->hover_page = page;
		s->hover_slot = slot;
		s->hover_valid = 1;
	}
	else
		s->hover_valid = 0;
}

int INV_ScreenKey (inv_screen_t *s, int key)
{
	/* Arrow keys walk the backpack grid linearly.  Enter/right mouse use
	 * the selected cell; menu.c owns the equip/drop/Tab keys. */
	switch (key)
	{
	case K_UPARROW:
	case K_DOWNARROW:
	case K_LEFTARROW:
	case K_RIGHTARROW:
		{
			int li = Inv_ScreenIndexFor (s);
			int next = li;
			if (key == K_DOWNARROW)
				next = li + 1;
			else if (key == K_UPARROW)
				next = li - 1;
			else if (key == K_RIGHTARROW)
				next = li + INV_GRID_COLS;
			else
				next = li - INV_GRID_COLS;
			if (next < 0)
				next += INV_BACKPACK_CELLS;
			else if (next >= INV_BACKPACK_CELLS)
				next -= INV_BACKPACK_CELLS;
			{
				int page, slot;
				Inv_CellForLocal (next, &page, &slot);
				s->page = page;
				s->slot = slot;
				s->hover_valid = 0;
			}
			return 1;
		}
	case K_ENTER:
		CL_SendInventoryAction (INVA_USE, s->page, s->slot, 0);
		return 1;
	default:
		return 0;
	}
}
