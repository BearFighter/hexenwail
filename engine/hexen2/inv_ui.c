/* inv_ui.c -- Diablo-style inventory overlay (docs/items-inventory-spec.md).
 *
 * Real-time overlay: the menu layer keeps key_dest == key_menu so gameplay
 * input is swallowed, but the server is deliberately not paused.  This file
 * owns the layout, hit-testing and drawing behind m_inventory.
 *
 * The static art is a single baked LMP (gfx/items/inventory_bg.lmp).  This
 * module only draws live state over it: item art, hover/selection/drag frames,
 * empty-socket tags and the detail text.
 *
 * Layout (CANVAS_MENU units, 320x200):
 *   - title strip at the top,
 *   - page-0 equipment slots 0-15 arranged as a Diablo-style paper doll on the
 *     left; the two weapon slots are full-height, not row-of-boxes icons,
 *   - backpack grid of the 60 cells (page 0 slots 16-31, pages 1-2 slots
 *     0-31), ten columns wide and six rows tall, on the right,
 *   - a detail strip at the bottom with the registry name and compact stats.
 */

#include "quakedef.h"
#include "cvar.h"
#include "inv_ui.h"
#include "draw.h"
#include "menu.h"
#include "client.h"

/* Screen geometry in CANVAS_MENU units (320x200). */
#define	INV_CANVAS_W		320
#define	INV_CANVAS_H		200
#define	INV_TITLE_X		240
#define	INV_TITLE_Y		4
#define	INV_TIP_X		8
#define	INV_TIP_Y		30
#define	INV_DOLL_X		0
#define	INV_DOLL_Y		44
#define	INV_DOLL_W		120
#define	INV_DOLL_H		150
#define	INV_GRID_X		126
#define	INV_GRID_Y		58
#define	INV_GRID_PITCH	19
#define	INV_GRID_COLS	10
#define	INV_GRID_ROWS	6
#define	INV_CELL_ICON	17
#define	INV_DETAIL_Y	180
#define	INV_DETAIL_H	20
#define	INV_BACKPACK_CELLS	(INV_GRID_COLS * INV_GRID_ROWS)
#define	INV_OCCUPIED_ID		255	/* covered cell of a multi-cell item */
#define	INV_BACKGROUND_PIC	"gfx/items/inventory_bg.lmp"

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
	s->drag_valid = 0;
	s->drag_page = -1;
	s->drag_slot = -1;
	s->drop_valid = 0;
	s->drop_page = -1;
	s->drop_slot = -1;
	s->pointer_x = 0;
	s->pointer_y = 0;
}

void INV_ScreenShutdown (inv_screen_t *s)
{
	s->client = 0;
	s->hover_valid = 0;
	s->drag_valid = 0;
	s->drop_valid = 0;
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

/* Item registry strings are "path.lmp[#WxH][#Display Name][|Tooltip]".
 * The '|' suffix is the compact catalog line shown in the detail strip.  Both
 * suffixes ride along the existing ex-item registration, so no protocol field
 * is added.  Sbar_ItemIconPath strips everything from the first '#'. */
static void Inv_ParseIconString (const char *src, char *path, int size,
				 int *w, int *h, char *name, int name_size,
				 char *tooltip, int tooltip_size)
{
	int n = 0;
	const char *p;
	int tw = 0, th = 0, digits = 0;

	*w = 1;
	*h = 1;
	if (name)
		name[0] = 0;
	if (tooltip)
		tooltip[0] = 0;
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

	p = src + n;
	if (*p != '#')
		return;
	p++;
	digits = 0;
	while (*p >= '0' && *p <= '9')
	{
		tw = tw * 10 + (*p - '0');
		p++;
		digits++;
	}
	if (digits && *p == 'x')
	{
		p++;
		digits = 0;
		while (*p >= '0' && *p <= '9')
		{
			th = th * 10 + (*p - '0');
			p++;
			digits++;
		}
		if (digits && tw >= 1 && th >= 1 && (!*p || *p == '#' || *p == '|'))
		{
			*w = tw > INV_GRID_COLS ? INV_GRID_COLS : tw;
			*h = th > INV_GRID_ROWS ? INV_GRID_ROWS : th;
			if (*p == '#')
				p++;
		}
	}
	if (name)
	{
		n = 0;
		while (*p && *p != '|' && n + 1 < name_size)
		{
			name[n] = *p;
			p++;
			n++;
		}
		name[n] = 0;
	}
	if (tooltip && *p == '|')
	{
		p++;
		n = 0;
		while (*p && n + 1 < tooltip_size)
		{
			tooltip[n] = *p;
			p++;
			n++;
		}
		tooltip[n] = 0;
	}
}

/* Item id -> icon path, footprint, display name and compact catalog line
 * through the ex item list. */
static void Inv_ItemInfo (client_state_t *client, float item_id,
			  char *path, int size, int *w, int *h,
			  char *name, int name_size,
			  char *tooltip, int tooltip_size)
{
	int i;

	path[0] = 0;
	*w = 1;
	*h = 1;
	if (name)
		name[0] = 0;
	if (tooltip)
		tooltip[0] = 0;
	if (!client)
		return;
	for (i = 0; i < client->num_ex_items; i++)
	{
		if (client->ex_items[i].id == (int)item_id)
		{
			Inv_ParseIconString (client->ex_items[i].icon,
					     path, size, w, h, name, name_size,
					     tooltip, tooltip_size);
			return;
		}
	}
}

/* Draw the whole item icon into its rectangle.  A multi-cell item is one
 * picture across the whole footprint, not a stack of unrelated cell icons. */
static void Inv_DrawItemArt (client_state_t *client, inv_rect_t r,
			     float item_id, float count)
{
	char	path[MAX_QPATH];
	char	name[64];
	int	w, h;
	qpic_t	*pic;

	Inv_ItemInfo (client, item_id, path, sizeof(path), &w, &h,
		      name, sizeof(name), NULL, 0);
	(void)name;
	if (!path[0])
		return;
	pic = Draw_CachePic (path);
	if (!pic || pic->width <= 0 || pic->height <= 0)
		return;

	/* Icons are authored above their display size so the 2x inventory canvas
	 * does not turn them into four-pixel blocks.  Always stretch into the
	 * footprint; a 1x1 item simply has source and destination both square. */
	Draw_PicStretch (r.x, r.y, r.w, r.h, pic);

	if (count > 1)
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
	char name[64];

	*w = 1;
	*h = 1;
	if (anchor < 0 || anchor >= INV_BACKPACK_CELLS)
		return;

	Inv_CellForLocal (anchor, &page, &slot);
	id = Inv_Find (client, page, slot, &count);
	if (id <= 0 || (int)id == INV_OCCUPIED_ID)
		return;

	Inv_ItemInfo (client, id, path, sizeof(path), w, h,
		      name, sizeof(name), NULL, 0);
	(void)name;
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

/* A major rune spans a two-cell equipment column.  Its lower cell carries the
 * occupied marker with the anchor equip slot in .count; treat either half as
 * the item so hover, selection and dragging all pick up the whole rune. */
static void Inv_ResolveEquipRune (client_state_t *client, int page, int slot,
				  int *outpage, int *outslot)
{
	float count;
	float id;

	*outpage = page;
	*outslot = slot;
	if (page != 0 || slot < 0 || slot >= INV_SCREEN_EQUIP_SLOTS)
		return;
	id = Inv_Find (client, page, slot, &count);
	if ((int)id == INV_OCCUPIED_ID &&
	    (slot == 13 || slot == 15) && (int)count == slot - 1)
		*outslot = slot - 1;
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

/* The hovered item wins; otherwise show the selected item's registry name and
 * compact catalog line. */
static void Inv_ItemNameForScreen (const inv_screen_t *s,
				   char *name, int name_size,
				   char *tooltip, int tooltip_size)
{
	int page = s->page, slot = s->slot;
	char path[MAX_QPATH];
	float count;
	float id;
	int w, h, li, anchor;

	name[0] = 0;
	if (tooltip)
		tooltip[0] = 0;
	if (s->hover_valid)
	{
		page = s->hover_page;
		slot = s->hover_slot;
	}
	/* Covered cells resolve to the item's anchor before the registry read. */
	Inv_ResolveEquipRune (s->client, page, slot, &page, &slot);
	li = Inv_LocalForCell (page, slot);
	if (li >= 0)
	{
		anchor = Inv_AnchorForCell (s->client, li);
		if (anchor >= 0)
			Inv_CellForLocal (anchor, &page, &slot);
	}
	id = Inv_Find (s->client, page, slot, &count);
	if (id <= 0)
		return;
	Inv_ItemInfo (s->client, id, path, sizeof(path), &w, &h,
		      name, name_size, tooltip, tooltip_size);
	(void)path;
	(void)w;
	(void)h;
}

/* Page-0 equipment layout.  Slots 4 and 5 are the main-hand/off-hand
 * paper-doll sockets.  Slots 10-11 are the guaranteed minor runes; each
 * following pair of cells is one split-capable major rune column — a 1x2
 * major rune occupies both cells, two 1x1 minor runes occupy them separately.
 * Rectangles are deliberately disjoint: no socket overlaps another. */
static const inv_rect_t inv_equip_rects[INV_SCREEN_EQUIP_SLOTS] =
{
	/* 0 helmet          */	{ 50,  46, 20, 16 },
	/* 1 body armor      */	{ 46,  76, 28, 28 },
	/* 2 bracers         */	{ 20, 106, 18, 12 },
	/* 3 amulet          */	{ 54,  64, 12, 10 },
	/* 4 main hand       */	{ 94,  58, 24, 66 },
	/* 5 off hand        */	{  2,  58, 24, 66 },
	/* 6 ring 1          */	{ 20, 126, 12, 12 },
	/* 7 ring 2          */	{ 88, 126, 12, 12 },
	/* 8 boots           */	{ 46, 124, 28, 14 },
	/* 9 belt            */	{ 46, 106, 28,  8 },
	/* 10 minor rune 1   */	{  2, 148, 14, 14 },
	/* 11 minor rune 2   */	{104, 148, 14, 14 },
	/* 12 major rune A-1 */	{ 46, 146, 14, 14 },
	/* 13 major rune A-2 */	{ 46, 162, 14, 14 },
	/* 14 major rune B-1 */	{ 66, 146, 14, 14 },
	/* 15 major rune B-2 */	{ 66, 162, 14, 14 },
};

static inv_rect_t Inv_EquipmentRect (int slot)
{
	if (slot < 0 || slot >= INV_SCREEN_EQUIP_SLOTS)
		slot = 0;
	return inv_equip_rects[slot];
}

/* Hover label for an equipment cell.  Drawn above the doll so an empty slot
 * still says what it is. */
static const char *const inv_equip_labels[INV_SCREEN_EQUIP_SLOTS] =
{
	"Helmet", "Body Armor", "Bracers", "Amulet",
	"Main Hand", "Off Hand", "Ring 1", "Ring 2",
	"Boots", "Belt", "Minor Rune 1", "Minor Rune 2",
	"Major Rune A (upper)", "Major Rune A (lower)",
	"Major Rune B (upper)", "Major Rune B (lower)",
};

static const char *Inv_SlotLabel (int slot)
{
	if (slot < 0 || slot >= INV_SCREEN_EQUIP_SLOTS)
		return "Backpack";
	return inv_equip_labels[slot];
}

/* Tiny always-on tags for empty sockets.  The full name is still shown on
 * hover; these just stop the doll from looking like unlabelled boxes. */
static const char *const inv_equip_tags[INV_SCREEN_EQUIP_SLOTS] =
{
	"HEL", "ARM", "BR", "AMU",
	"MAIN", "OFF", "R1", "R2",
	"BOT", "BELT", "r1", "r2",
	"A1", "A2", "B1", "B2",
};

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

/* One continuous border around a whole footprint.  Backpack cells only get
 * plain frames here; selection and hover are drawn once per item in pass 2 so
 * a 3x2 item cannot show six independent "selected" cells. */
static void Inv_DrawSpanBorder (inv_rect_t r, int selected, int hovered)
{
	float cr = hovered ? 0.95f : 0.35f;
	float cg = hovered ? 0.85f : 0.70f;
	float cb = hovered ? 0.35f : 1.00f;
	float ca = 1.0f;

	Inv_FillRect (r.x - 2, r.y - 2, r.w + 4, 1, cr, cg, cb, ca);
	Inv_FillRect (r.x - 2, r.y + r.h + 1, r.w + 4, 1, cr, cg, cb, ca);
	Inv_FillRect (r.x - 2, r.y - 2, 1, r.h + 4, cr, cg, cb, ca);
	Inv_FillRect (r.x + r.w + 1, r.y - 2, 1, r.h + 4, cr, cg, cb, ca);
}

/* Draw one cell of the grid.  Equipment cells keep their own highlight;
 * backpack cells stay neutral so item art draws over one clean grid. */
static void Inv_DrawCell (const inv_screen_t *s, inv_rect_t r,
			  int page, int slot)
{
	int li = Inv_LocalForCell (page, slot);
	int selected = 0, hovered = 0;

	if (li < 0)
	{
		selected = (s->page == page && s->slot == slot);
		hovered = (s->hover_valid && s->hover_page == page &&
			   s->hover_slot == slot);
	}

	/* Neutral socket art is baked into inventory_bg.lmp.  Only live state
	 * gets a runtime frame, so empty slots no longer look like floating
	 * rectangles. */
	if (selected || hovered)
		Inv_DrawCellFrame (r, selected, hovered);
}

/* Draw an item once, from its anchor, as one picture across its footprint.
 * Selection and hover are one border around that whole picture.  Covered-cell
 * markers only matter for hit-testing, never for art. */
static void Inv_DrawItemSpan (const inv_screen_t *s, inv_rect_t r,
			      int page, int slot)
{
	float count = 0;
	float id = Inv_Find (s->client, page, slot, &count);
	int li = Inv_LocalForCell (page, slot);
	int anchor, w = 1, h = 1;
	int apage, aslot, aid;
	float acount = 0;

	if (id <= 0)
		return;

	if (li < 0)
	{
		inv_rect_t art = r;
		float next_count = 0;
		float next_id;

		/* The lower half of a major rune has no art of its own. */
		if ((int)id == INV_OCCUPIED_ID)
			return;

		/* A major rune draws once across both cells of its column. */
		if (slot == 12 || slot == 14)
		{
			next_id = Inv_Find (s->client, 0, slot + 1,
					    &next_count);
			if ((int)next_id == INV_OCCUPIED_ID &&
			    (int)next_count == slot)
			{
				inv_rect_t lower = Inv_EquipmentRect (slot + 1);
				art.h = lower.y + lower.h - art.y;
				if ((s->page == 0 && (s->slot == slot ||
				     s->slot == slot + 1)) ||
				    (s->hover_valid && s->hover_page == 0 &&
				     (s->hover_slot == slot ||
				      s->hover_slot == slot + 1)))
					Inv_DrawSpanBorder (art,
						s->page == 0 && (s->slot == slot ||
						s->slot == slot + 1),
						s->hover_valid && s->hover_page == 0 &&
						(s->hover_slot == slot ||
						 s->hover_slot == slot + 1));
			}
		}
		/* Paper-doll socket: one full icon scaled to the whole field. */
		Inv_DrawItemArt (s->client, art, id, count);
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
	r.w = INV_CELL_ICON + (w - 1) * INV_GRID_PITCH;
	r.h = INV_CELL_ICON + (h - 1) * INV_GRID_PITCH;

	{
		int sel_anchor = Inv_SelectionAnchor (s);
		int hov_anchor = Inv_HoverAnchor (s);
		if (sel_anchor == anchor || hov_anchor == anchor)
			Inv_DrawSpanBorder (r, sel_anchor == anchor,
					    hov_anchor == anchor);
	}

	Inv_DrawItemArt (s->client, r, aid, acount);
}

void INV_ScreenDraw (const inv_screen_t *s)
{
	int slot, li;
	qpic_t *bg = Draw_CachePic (INV_BACKGROUND_PIC);

	/* One baked backdrop replaces the old floating rectangles: textured
	 * leather/wood, carved sockets, silhouette and detail plate.  If the
	 * asset is missing, fall back to a flat dark field so text stays legible. */
	if (bg && bg->width >= INV_CANVAS_W && bg->height >= INV_CANVAS_H)
		Draw_PicStretch (0, 0, INV_CANVAS_W, INV_CANVAS_H, bg);
	else
		Inv_FillRect (0, 0, INV_CANVAS_W, INV_CANVAS_H,
			      0.02f, 0.025f, 0.02f, 0.92f);

	M_Print (INV_TITLE_X, INV_TITLE_Y, "Inventory");
	M_Print (INV_TIP_X, INV_TIP_Y,
		 "LMB sel RMB use E equip D drop TAB exit");

	/* Pass 1: live highlights over the baked socket art. */
	for (slot = 0; slot < INV_SCREEN_EQUIP_SLOTS; slot++)
		Inv_DrawCell (s, Inv_EquipmentRect (slot), 0, slot);

	/* Empty sockets carry a short tag so the silhouette's slots are readable
	 * without hovering.  Item art in pass 2 covers the tag when occupied. */
	for (slot = 0; slot < INV_SCREEN_EQUIP_SLOTS; slot++)
	{
		float count = 0;
		float id = Inv_Find (s->client, 0, slot, &count);
		if (id <= 0)
		{
			inv_rect_t r = Inv_EquipmentRect (slot);
			const char *tag = inv_equip_tags[slot];
			int tw = (int)strlen (tag) * 6;
			Draw_SmallString (r.x + (r.w - tw) / 2,
					  r.y + (r.h - 6) / 2, tag);
		}
	}

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

	/* Hovering an equipment cell always says what that cell is, even when
	 * it is empty.  Backpack cells use the detail strip below instead. */
	if (s->hover_valid && s->hover_page == 0 &&
	    s->hover_slot >= 0 && s->hover_slot < INV_SCREEN_EQUIP_SLOTS)
		Draw_SmallString (6, 18, Inv_SlotLabel (s->hover_slot));

	/* Detail strip: item identity first; stats/tooltip are added below it
	 * from the same registry string that supplies the icon. */
	{
		char name[64];
		char tooltip[64];
		Inv_ItemNameForScreen (s, name, sizeof(name),
				       tooltip, sizeof(tooltip));
		Draw_SmallString (8, INV_DETAIL_Y + 4,
				  name[0] ? name : "No item selected");
		if (tooltip[0])
			Draw_SmallString (8, INV_DETAIL_Y + 14, tooltip);
	}

	/* Dragged item: mark the resolved target, then draw a small ghost at
	 * the pointer so the gesture is obvious. */
	if (s->drag_valid)
	{
		if (s->drop_valid)
		{
			inv_rect_t target;
			int valid = 0;
			if (s->drop_page == 0 &&
			    s->drop_slot >= 0 &&
			    s->drop_slot < INV_SCREEN_EQUIP_SLOTS)
			{
				target = Inv_EquipmentRect (s->drop_slot);
				valid = 1;
			}
			else
			{
				int li = Inv_LocalForCell (s->drop_page, s->drop_slot);
				if (li >= 0)
				{
					target = Inv_BackpackRect (li);
					valid = 1;
				}
			}
			if (valid)
				Inv_DrawCellFrame (target, 0, 1);
		}
		{
			float count = 0;
			float id = Inv_Find (s->client, s->drag_page,
					     s->drag_slot, &count);
			if (id > 0)
			{
				inv_rect_t ghost;
				ghost.x = s->pointer_x - 9;
				ghost.y = s->pointer_y - 9;
				ghost.w = 18;
				ghost.h = 18;
				Inv_DrawItemArt (s->client, ghost, id, count);
			}
		}
	}
}

/* Hit-test both the paper doll and the backpack grid, including empty
 * cells so clicking a cell always moves the selection. */
static int Inv_CellAt (const inv_screen_t *s, int vx, int vy,
		       int *page, int *slot)
{
	{
		int equip_slot;
		for (equip_slot = 0; equip_slot < INV_SCREEN_EQUIP_SLOTS;
		     equip_slot++)
		{
			inv_rect_t r = Inv_EquipmentRect (equip_slot);
			if (vx >= r.x - 2 && vx < r.x + r.w + 2 &&
			    vy >= r.y - 2 && vy < r.y + r.h + 2)
			{
				*page = 0;
				*slot = equip_slot;
				return 1;
			}
		}
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
	Inv_ResolveEquipRune (s->client, page, slot, &page, &slot);
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
		Inv_ResolveEquipRune (s->client, page, slot, &page, &slot);
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

/* Resolve a pointer cell the same way click/hover do: covered cells and the
 * lower half of a major rune resolve to their item anchor. */
static void Inv_ResolveCell (inv_screen_t *s, int vx, int vy,
			     int *page, int *slot)
{
	int li, anchor;
	if (!Inv_CellAt (s, vx, vy, page, slot))
	{
		*page = -1;
		*slot = -1;
		return;
	}
	Inv_ResolveEquipRune (s->client, *page, *slot, page, slot);
	li = Inv_LocalForCell (*page, *slot);
	anchor = Inv_AnchorForCell (s->client, li);
	if (anchor >= 0)
		Inv_CellForLocal (anchor, page, slot);
}

void INV_ScreenDragStart (inv_screen_t *s, int vx, int vy)
{
	float count;
	float id;

	s->pointer_x = vx;
	s->pointer_y = vy;
	/* A press always selects the cell, including covered cells of a span. */
	INV_ScreenClick (s, vx, vy);

	id = Inv_Find (s->client, s->page, s->slot, &count);
	if (id <= 0 || (int)id == INV_OCCUPIED_ID)
		return;

	s->drag_valid = 1;
	s->drag_page = s->page;
	s->drag_slot = s->slot;
	s->drop_valid = 0;
	s->drop_page = -1;
	s->drop_slot = -1;
}

void INV_ScreenDragThink (inv_screen_t *s, int vx, int vy, int down)
{
	int page, slot;

	s->pointer_x = vx;
	s->pointer_y = vy;
	if (!s->drag_valid)
		return;

	if (down)
	{
		Inv_ResolveCell (s, vx, vy, &page, &slot);
		s->drop_valid = page >= 0;
		s->drop_page = page;
		s->drop_slot = slot;
		return;
	}

	/* Button released: commit the drop, then clear all drag state. */
	Inv_ResolveCell (s, vx, vy, &page, &slot);
	if (page >= 0 &&
	    (page != s->drag_page || slot != s->drag_slot))
	{
		int src_equip = (s->drag_page == 0 &&
				 s->drag_slot < INV_SCREEN_EQUIP_SLOTS);
		int dst_equip = (page == 0 && slot < INV_SCREEN_EQUIP_SLOTS);

		if (!src_equip && !dst_equip)
			CL_SendInventoryAction (INVA_MOVE, s->drag_page,
						s->drag_slot,
						page * INV_SCREEN_SLOTS + slot);
		else if (!src_equip && dst_equip)
			CL_SendInventoryAction (INVA_EQUIP, s->drag_page,
						s->drag_slot, slot);
		else if (src_equip && !dst_equip)
			CL_SendInventoryAction (INVA_UNEQUIP, s->drag_slot, 0, 0);
	}
	s->drag_valid = 0;
	s->drag_page = -1;
	s->drag_slot = -1;
	s->drop_valid = 0;
	s->drop_page = -1;
	s->drop_slot = -1;
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
				next = li + INV_GRID_COLS;
			else if (key == K_UPARROW)
				next = li - INV_GRID_COLS;
			else if (key == K_RIGHTARROW)
				next = li + 1;
			else
				next = li - 1;
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
