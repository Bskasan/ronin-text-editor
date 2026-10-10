// Inventory.cs: a record, an enum, a switch expression and LINQ.
using System;
using System.Collections.Generic;
using System.Linq;

namespace Samples
{
    public enum Rarity { Common, Rare, Legendary }

    public record Item(string Name, int Count, Rarity Rarity);

    public static class Inventory
    {
        /* Gold value of one item. Legendary items are priced
           by hand, everything else by its rarity. */
        static decimal Price(Item item) => item.Rarity switch
        {
            Rarity.Common => 1.5m,
            Rarity.Rare => 12m,
            _ => 250m,
        };

        public static void Main()
        {
            var items = new List<Item>
            {
                new("rope", 3, Rarity.Common),
                new("lantern", 1, Rarity.Rare),
                new("crown", 1, Rarity.Legendary),
            };
            decimal total = 0;
            foreach (var item in items.Where(i => i.Count > 0))
            {
                total += Price(item) * item.Count;
                Console.WriteLine($"{item.Name,-8} x{item.Count}");
            }
            string? note = total > 0x100 ? @"rich\list" : null;
            Console.WriteLine($"total {total:F2} {note ?? "-"}");
        }
    }
}
