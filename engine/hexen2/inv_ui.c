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
#define	INV_FOOTPRINT_BASE	90	/* anchor count = 90 + w*8 + h */
#define	INV_FOOTPRINT_STRIDE	8

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

/* Item id -> icon path through the client's registered ex item list. */
static void Inv_ItemIcon (client_state_t *client, float item_id, char *path, int size)
{
	int i;
	path[0] = 0;
	if (!client)
		return;
	for (i = 0; i < client->num_ex_items; i++)
	{
		if (client->ex_items[i].id == (int)item_id)
		{
			int n = 0;
			const char *src = client->ex_items[i].icon;
			while (n + 1 < size && src[n])
			{
				path[n] = src[n];
				n++;
			}
			path[n] = 0;
			return;
		}
	}
}

/* A scaled copy of the icon, so a 3x1 sword fills three cells lengthwise.
 * Draw_CachePicResize keys its cache by path alone and would thrash between
 * sizes in one frame, so scale into our own buffer instead. */
typedef struct inv_scaled_pic_s
{
	qpic_t	pic;
	byte	data[128 * 64];
} inv_scaled_pic_t;

static inv_scaled_pic_t inv_scaled_pic;

static qpic_t *Inv_ScaledPic (qpic_t *src, int w, int h)
{
	int x, y;

	if (!src || src->width <= 0 || src->height <= 0)
		return NULL;
	if (w < 1 || h < 1 || w > 128 || h > 64)
		return NULL;

	inv_scaled_pic.pic.width = w;
	inv_scaled_pic.pic.height = h;
	for (y = 0; y < h; y++)
	{
		int sy = y * src->height / h;
		for (x = 0; x < w; x++)
		{
			int sx = x * src->width / w;
			inv_scaled_pic.pic.data[y * w + x] =
				src->data[sy * src->width + sx];
		}
	}
	return &inv_scaled_pic.pic;
}

static void Inv_DrawItem (client_state_t *client, inv_rect_t r,
			  float item_id, float count)
{
	char	path[MAX_QPATH];
	qpic_t	*pic, *scaled;

	path[0] = 0;
	if (item_id > 0)
		Inv_ItemIcon (client, item_id, path, sizeof(path));
	if (!path[0])
		return;
	pic = Draw_CachePic (path);
	if (!pic || pic->width <= 0 || pic->height <= 0)
		return;

	scaled = Inv_ScaledPic (pic, r.w, r.h);
	if (scaled)
		Draw_Pic (r.x, r.y, scaled);
	else
		Draw_SubPic (r.x, r.y, pic, 0, 0, r.w, r.h);

	if (count > 1 && count < INV_FOOTPRINT_BASE)
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

/* The anchor's count carries its footprint (64 + w*8 + h).  Reading it here
 * keeps the UI correct even when a covered-cell marker is missing on the wire. */
static void Inv_SpanForAnchor (client_state_t *client, int anchor,
			       int *w, int *h)
{
	int page, slot, encoded;
	float count;

	*w = 1;
	*h = 1;
	if (anchor < 0 || anchor >= INV_BACKPACK_CELLS)
		return;

	Inv_CellForLocal (anchor, &page, &slot);
	if (Inv_Find (client, page, slot, &count) <= 0 ||
	    count < INV_FOOTPRINT_BASE)
		return;

	encoded = (int)count - INV_FOOTPRINT_BASE;
	*w = encoded / INV_FOOTPRINT_STRIDE;
	*h = encoded % INV_FOOTPRINT_STRIDE;
	if (*w < 1) *w = 1;
	if (*w > INV_GRID_COLS) *w = INV_GRID_COLS;
	if (*h < 1) *h = 1;
	if (*h > INV_GRID_ROWS) *h = INV_GRID_ROWS;
}

/* Resolve a covered cell to the anchor whose footprint contains it. */
static int Inv_AnchorForCell (client_state_t *client, int li)
{
	int i, arow, acol, row, col, w, h;

	if (li < 0 || li >= INV_BACKPACK_CELLS)
		return -1;

	row = li / INV_GRID_COLS;
	col = li % INV_GRID_COLS;

	for (i = 0; i < INV_BACKPACK_CELLS; i++)
	{
		int page, slot;
		float count;
		Inv_CellForLocal (i, &page, &slot);
		if (Inv_Find (client, page, slot, &count) <= 0 ||
		    count < INV_FOOTPRINT_BASE)
			continue;
		Inv_SpanForAnchor (client, i, &w, &h);
		arow = i / INV_GRID_COLS;
		acol = i % INV_GRID_COLS;
		if (row >= arow && row < arow + h &&
		    col >= acol && col < acol + w)
			return i;
	}
	return -1;
}

/* True when the selected or hovered cell sits inside this item's footprint. */
static int Inv_SelectionInSpan (const inv_screen_t *s, int anchor, int w, int h)
{
	int li, arow, acol, row, col;

	li = Inv_LocalForCell (s->page, s->slot);
	if (li < 0 || anchor < 0 || anchor >= INV_BACKPACK_CELLS)
		return 0;
	arow = anchor / INV_GRID_COLS;
	acol = anchor % INV_GRID_COLS;
	row = li / INV_GRID_COLS;
	col = li % INV_GRID_COLS;
	return row >= arow && row < arow + h && col >= acol && col < acol + w;
}

static int Inv_HoverInSpan (const inv_screen_t *s, int anchor, int w, int h)
{
	int li, arow, acol, row, col;

	if (!s->hover_valid)
		return 0;
	li = Inv_LocalForCell (s->hover_page, s->hover_slot);
	if (li < 0 || anchor < 0 || anchor >= INV_BACKPACK_CELLS)
		return 0;
	arow = anchor / INV_GRID_COLS;
	acol = anchor % INV_GRID_COLS;
	row = li / INV_GRID_COLS;
	col = li % INV_GRID_COLS;
	return row >= arow && row < arow + h && col >= acol && col < acol + w;
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
 * a multi-cell item, so the rows stay intact. */
static void Inv_DrawCell (const inv_screen_t *s, inv_rect_t r,
			  int page, int slot)
{
	int li = Inv_LocalForCell (page, slot);
	int w = 1, h = 1;

	if (li >= 0)
		Inv_SpanForAnchor (s->client, li, &w, &h);

	Inv_DrawCellFrame (r,
			   li >= 0 ? Inv_SelectionInSpan (s, li, w, h) :
				     (s->page == page && s->slot == slot),
			   li >= 0 ? Inv_HoverInSpan (s, li, w, h) :
				     (s->hover_valid && s->hover_page == page &&
				      s->hover_slot == slot));
}

/* Draw an item over its whole footprint, once, from its anchor cell. */
static void Inv_DrawItemSpan (const inv_screen_t *s, inv_rect_t r,
			      int page, int slot)
{
	float count = 0;
	float id = Inv_Find (s->client, page, slot, &count);
	int li = Inv_LocalForCell (page, slot);
	int anchor, w = 1, h = 1;

	if (id <= 0)
		return;
	if (li >= 0)
	{
		anchor = Inv_AnchorForCell (s->client, li);
		if (anchor >= 0 && anchor != li)
			return;
		Inv_SpanForAnchor (s->client, li, &w, &h);
	}
	else
	{
		/* Equipment cells have no footprint. */
		w = 1;
		h = 1;
	}

	r.w = INV_CELL_ICON + (w - 1) * INV_GRID_PITCH;
	r.h = INV_CELL_ICON + (h - 1) * INV_GRID_PITCH;
	Inv_DrawItem (s->client, r, id, count);
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
