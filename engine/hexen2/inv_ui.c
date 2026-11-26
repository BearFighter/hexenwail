/* inv_ui.c -- Diablo-style inventory screen (docs/items-inventory-spec.md).
 *
 * Phase 1 of the inventory slice: the read-only screen over the protocol-21
 * extended inventory.  The engine menu state (m_inventory) owns the state
 * machine; this file owns the layout, hit-testing and drawing behind it.
 *
 * Layout (CANVAS_MENU units, 320x200):
 *   - title bar at the top,
 *   - paper doll (page 0 equipment slots, ascending) down the left,
 *   - backpack grid of the 60 cells (page 0 slots 16-31, pages 1-2 slots
 *     0-31) below it, ten columns wide and six rows tall.
 */

#include "inv_ui.h"
#include "quakedef.h"
#include "draw.h"
#include "menu.h"

/* Screen geometry in CANVAS_MENU units (320x200). */
#define	INV_TITLE_X		120
#define	INV_TITLE_Y		8
#define	INV_DOLL_X		16
#define	INV_DOLL_Y		24
#define	INV_DOLL_PITCH	32
#define	INV_DOLL_ICON	28
#define	INV_GRID_X		16
#define	INV_GRID_Y		56
#define	INV_GRID_PITCH	24
#define	INV_GRID_COLS	10
#define	INV_GRID_ROWS	6
#define	INV_BACKPACK_CELLS	60
#define	INV_SCREEN_MAX_Y	200

void INV_ScreenInit (inv_screen_t *s, client_t *client)
{
	s->client = client;
	s->page = 0;
	s->slot = 16;
}

void INV_ScreenShutdown (inv_screen_t *s)
{
	s->client = 0;
}

/* Read an item out of the client's extended-inventory chain: page, slot ->
 * item id + count.  Returns the item id; 0 means empty.  Same walk the inv_*
 * slot builtins use, so the screen, the action queue and the storage cannot
 * disagree about what an item is. */
static float Inv_Find (client_t *client, int page, int slot, float *count)
{
	ex_inventory_page_t *p = client->ex_inventory;
	int i;
	for (i = 0; i < page; i++)
	{
		if (!p)
			return 0;
		p = p->next;
	}
	if (!p)
	{
		*count = 0;
		return 0;
	}
	if (slot < 0 || slot >= INV_SCREEN_SLOTS)
	{
		*count = 0;
		return 0;
	}
	if (p->item_id[slot] == 0)
	{
		*count = 0;
		return 0;
	}
	*count = p->item_cnt[slot];
	return p->item_id[slot];
}

/* Item id -> icon path through the client's registered ex item list. */
static void Inv_ItemIcon (client_t *client, float item_id, char *path, int size)
{
	int i;
	path[0] = 0;
	for (i = 0; i < client->num_ex_items; i++)
	{
		if (client->ex_items[i].id == (int)item_id)
		{
			/* engine string copy, bounded */
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

static void Inv_DrawItem (client_t *client, int x, int y, float item_id, float count)
{
	char path[MAX_QPATH];
	qpic_t *pic;
	path[0] = 0;
	if (item_id > 0)
		Inv_ItemIcon (client, item_id, path, MAX_QPATH);
	if (path[0] == 0)
		return;
	pic = Draw_CachePic (path);
	if (!pic)
		return;
	Draw_Pic (x, y, pic);
	if (count > 1)
	{
		char buf[16];
		int n = 0;
		int v = (int)count;
		if (v == 0) v = 0;
		/* decimal */
		{
			char tmp[16];
			int len = 0;
			int val = v;
			if (val == 0) tmp[len++] = '0';
			while (val > 0)
			{
				tmp[len++] = (char)('0' + (val % 10));
				val /= 10;
			}
			/* reverse into buf */
			for (int i = 0; i < len; i++)
				buf[i] = tmp[len - 1 - i];
			buf[len] = 0;
		}
		M_Print (x, y + INV_GRID_PITCH, buf);
	}
}

void INV_ScreenDraw (const inv_screen_t *s)
{
	client_t *client = s->client;
	int li;

	M_Print (INV_TITLE_X, INV_TITLE_Y, "Inventory");

	/* Paper doll: equipped slots (page 0, slots 0-12), ascending. */
	{
		int y = INV_DOLL_Y;
		int slot;
		for (slot = 0; slot < INV_SCREEN_EQUIP_SLOTS; slot++)
		{
			float count = 0;
			float id = Inv_Find (client, 0, slot, &count);
			if (id <= 0)
				continue;
			Inv_DrawItem (client, INV_DOLL_X, y, id, count);
			y += INV_DOLL_PITCH;
			if (y + INV_DOLL_ICON > INV_SCREEN_MAX_Y)
				break;
		}
	}

	/* Backpack grid: the 60 cells. */
	for (li = 0; li < INV_BACKPACK_CELLS; li++)
	{
		int page, slot;
		float count = 0;
		float id;
		int col = li % INV_GRID_COLS;
		int row = li / INV_GRID_COLS;
		int x = INV_GRID_X + col * INV_GRID_PITCH;
		int y = INV_GRID_Y + row * INV_GRID_PITCH;
		if (li < 16)
		{
			page = 0;
			slot = 16 + li;
		}
		else
		{
			int l2 = li - 16;
			page = 1 + (l2 / 32);
			slot = l2 % 32;
		}
		id = Inv_Find (client, page, slot, &count);
		if (id <= 0)
			continue;
		Inv_DrawItem (client, x, y, id, count);
	}
}

int INV_ScreenClick (const inv_screen_t *s, int vx, int vy)
{
	client_t *client = s->client;
	int slot;

	for (slot = 0; slot < INV_SCREEN_EQUIP_SLOTS; slot++)
	{
		float count = 0;
		float id = Inv_Find (client, 0, slot, &count);
		if (id <= 0)
			continue;
		if (vx >= INV_DOLL_X && vx < INV_DOLL_X + INV_DOLL_ICON &&
			vy >= INV_DOLL_Y && vy < INV_DOLL_Y + INV_DOLL_ICON)
		{
			s->page = 0;
			s->slot = slot;
			return 1;
		}
	}

	{
		int li;
		for (li = 0; li < INV_BACKPACK_CELLS; li++)
		{
			int page, slot2;
			float count = 0;
			float id;
			int col = li % INV_GRID_COLS;
			int row = li / INV_GRID_COLS;
			int x = INV_GRID_X + col * INV_GRID_PITCH;
			int y = INV_GRID_Y + row * INV_GRID_PITCH;
			if (li < 16)
			{
				page = 0;
				slot2 = 16 + li;
			}
			else
			{
				int l2 = li - 16;
				page = 1 + (l2 / 32);
				slot2 = l2 % 32;
			}
			id = Inv_Find (client, page, slot2, &count);
			if (id <= 0)
				continue;
			if (vx >= x && vx < x + INV_GRID_PITCH &&
				vy >= y && vy < y + INV_GRID_PITCH)
			{
				s->page = page;
				s->slot = slot2;
				return 1;
			}
		}
	}
	return 0;
}

void INV_ScreenHover (const inv_screen_t *s, int vx, int vy)
{
	/* Phase 1: hover only moves the selection over an occupied cell. */
	INV_ScreenClick (s, vx, vy);
}

int INV_ScreenKey (const inv_screen_t *s, int key)
{
	/* Phase 1: keyboard navigation over the backpack grid.  Enter is
	 * consumed here; the actual use/equip action lands with the action queue
	 * (Phase 2). */
	switch (key)
	{
	case K_UPARROW:
	case K_DOWNARROW:
		{
			int li = Inv_ScreenIndexFor (s);
			int next = li;
			if (key == K_DOWNARROW)
				next = (li + 1 < INV_BACKPACK_CELLS) ? li + 1 : 0;
			else
				next = (li > 0) ? li - 1 : INV_BACKPACK_CELLS - 1;
			{
				int page, slot;
				Inv_CellForLocal (next, &page, &slot);
				s->page = page;
				s->slot = slot;
			}
			return 1;
		}
	case K_ENTER:
		/* Phase 2: selected (page, slot) -> clc_inv_action. */
		return 1;
	default:
		return 0;
	}
}
```

I referenced Inv_ScreenIndexFor and Inv_CellForLocal — need to define them:

```c
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
```

Hmm — Inv_ScreenHover calling Inv_ScreenClick means click handling on hover; fine for Phase 1 but hover should probably not consume... acceptable; note it.

Also `int n = 0; ... v == 0` weirdness — clean up the count formatting. Let me rewrite that part properly when I finalize: I'll just rewrite the whole file cleanly with write tool to avoid patch complexity.

Let me rewrite inv_ui.c completely and carefully.</think>Let me rewrite `inv_ui.c` cleanly (the first draft had messy count-formatting):<tool_call>write<arg_key>content</arg_key><arg_value>/* inv_ui.c -- Diablo-style inventory screen (docs/items-inventory-spec.md).
 *
 * Phase 1 of the inventory slice: the read-only screen over the protocol-21
 * extended inventory.  The engine menu state (m_inventory) owns the state
 * machine; this file owns the layout, hit-testing and drawing behind it.
 *
 * Layout (CANVAS_MENU units, 320x200):
 *   - title bar at the top,
 *   - paper doll (page 0 equipment slots, ascending) down the left,
 *   - backpack grid of the 60 cells (page 0 slots 16-31, pages 1-2 slots
 *     0-31) below it, ten columns wide and six rows tall.
 */

#include "inv_ui.h"
#include "quakedef.h"
#include "draw.h"
#include "menu.h"

/* Screen geometry in CANVAS_MENU units (320x200). */
#define	INV_TITLE_X		120
#define	INV_TITLE_Y		8
#define	INV_DOLL_X		16
#define	INV_DOLL_Y		24
#define	INV_DOLL_PITCH	32
#define	INV_DOLL_ICON	28
#define	INV_GRID_X		16
#define	INV_GRID_Y		56
#define	INV_GRID_PITCH	24
#define	INV_GRID_COLS	10
#define	INV_GRID_ROWS	6
#define	INV_BACKPACK_CELLS	60
#define	INV_SCREEN_MAX_Y	200

void INV_ScreenInit (inv_screen_t *s, client_t *client)
{
	s->client = client;
	s->page = 0;
	s->slot = 16;
}

void INV_ScreenShutdown (inv_screen_t *s)
{
	s->client = 0;
}

/* Read an item out of the client's extended-inventory chain: page, slot ->
 * item id + count.  Returns the item id; 0 means empty.  Same walk the inv_*
 * slot builtins use, so the screen, the action queue and the storage cannot
 * disagree about what an item is. */
static float Inv_Find (client_t *client, int page, int slot, float *count)
{
	ex_inventory_page_t *p = client->ex_inventory;
	int i;
	for (i = 0; i < page; i++)
	{
		if (!p)
			return 0;
		p = p->next;
	}
	if (!p)
	{
		*count = 0;
		return 0;
	}
	if (slot < 0 || slot >= INV_SCREEN_SLOTS)
	{
		*count = 0;
		return 0;
	}
	if (p->item_id[slot] == 0)
	{
		*count = 0;
		return 0;
	}
	*count = p->item_cnt[slot];
	return p->item_id[slot];
}

/* Item id -> icon path through the client's registered ex item list. */
static void Inv_ItemIcon (client_t *client, float item_id, char *path, int size)
{
	int i;
	path[0] = 0;
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

static void Inv_DrawItem (client_t *client, int x, int y, float item_id, float count)
{
	char path[MAX_QPATH];
	qpic_t *pic;
	path[0] = 0;
	if (item_id > 0)
		Inv_ItemIcon (client, item_id, path, MAX_QPATH);
	if (path[0] == 0)
		return;
	pic = Draw_CachePic (path);
	if (!pic)
		return;
	Draw_Pic (x, y, pic);
	if (count > 1)
	{
		char buf[16];
		int v = (int)count;
		int len = 0;
		char tmp[16];
		int val = v;
		int i;
		if (val == 0)
			tmp[len++] = '0';
		while (val > 0)
		{
			tmp[len++] = (char)('0' + (val % 10));
			val /= 10;
		}
		for (i = 0; i < len; i++)
			buf[i] = tmp[len - 1 - i];
		buf[len] = 0;
		M_Print (x, y + INV_GRID_PITCH, buf);
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

void INV_ScreenDraw (const inv_screen_t *s)
{
	client_t *client = s->client;
	int li;

	M_Print (INV_TITLE_X, INV_TITLE_Y, "Inventory");

	/* Paper doll: equipped slots (page 0, slots 0-12), ascending. */
	{
		int y = INV_DOLL_Y;
		int slot;
		for (slot = 0; slot < INV_SCREEN_EQUIP_SLOTS; slot++)
		{
			float count = 0;
			float id = Inv_Find (client, 0, slot, &count);
			if (id <= 0)
				continue;
			Inv_DrawItem (client, INV_DOLL_X, y, id, count);
			y += INV_DOLL_PITCH;
			if (y + INV_DOLL_ICON > INV_SCREEN_MAX_Y)
				break;
		}
	}

	/* Backpack grid: the 60 cells. */
	for (li = 0; li < INV_BACKPACK_CELLS; li++)
	{
		int page, slot;
		float count = 0;
		float id;
		int col = li % INV_GRID_COLS;
		int row = li / INV_GRID_COLS;
		int x = INV_GRID_X + col * INV_GRID_PITCH;
		int y = INV_GRID_Y + row * INV_GRID_PITCH;
		Inv_CellForLocal (li, &page, &slot);
		id = Inv_Find (client, page, slot, &count);
		if (id <= 0)
			continue;
		Inv_DrawItem (client, x, y, id, count);
	}
}

int INV_ScreenClick (const inv_screen_t *s, int vx, int vy)
{
	client_t *client = s->client;
	int slot;

	for (slot = 0; slot < INV_SCREEN_EQUIP_SLOTS; slot++)
	{
		float count = 0;
		float id = Inv_Find (client, 0, slot, &count);
		if (id <= 0)
			continue;
		if (vx >= INV_DOLL_X && vx < INV_DOLL_X + INV_DOLL_ICON &&
			vy >= INV_DOLL_Y && vy < INV_DOLL_Y + INV_DOLL_ICON)
		{
			s->page = 0;
			s->slot = slot;
			return 1;
		}
	}

	{
		int li;
		for (li = 0; li < INV_BACKPACK_CELLS; li++)
		{
			int page, slot2;
			float count = 0;
			float id;
			int col = li % INV_GRID_COLS;
			int row = li / INV_GRID_COLS;
			int x = INV_GRID_X + col * INV_GRID_PITCH;
			int y = INV_GRID_Y + row * INV_GRID_PITCH;
			Inv_CellForLocal (li, &page, &slot2);
			id = Inv_Find (client, page, slot2, &count);
			if (id <= 0)
				continue;
			if (vx >= x && vx < x + INV_GRID_PITCH &&
				vy >= y && vy < y + INV_GRID_PITCH)
			{
				s->page = page;
				s->slot = slot2;
				return 1;
			}
		}
	}
	return 0;
}

void INV_ScreenHover (const inv_screen_t *s, int vx, int vy)
{
	/* Phase 1: hover moves the selection over an occupied cell. */
	INV_ScreenClick (s, vx, vy);
}

int INV_ScreenKey (const inv_screen_t *s, int key)
{
	/* Phase 1: keyboard navigation over the backpack grid; Enter is consumed
	 * here and the use/equip action lands with the action queue in Phase 2. */
	switch (key)
	{
	case K_UPARROW:
	case K_DOWNARROW:
		{
			int li = Inv_ScreenIndexFor (s);
			int next = li;
			if (key == K_DOWNARROW)
				next = (li + 1 < INV_BACKPACK_CELLS) ? li + 1 : 0;
			else
				next = (li > 0) ? li - 1 : (INV_BACKPACK_CELLS - 1);
			{
				int page, slot;
				Inv_CellForLocal (next, &page, &slot);
				s->page = page;
				s->slot = slot;
			}
			return 1;
		}
	case K_ENTER:
		/* Phase 2: selected (page, slot) -> clc_inv_action. */
		return 1;
	default:
		return 0;
	}
}
