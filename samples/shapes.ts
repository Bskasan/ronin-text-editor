// shapes.ts: a tagged union, an interface and a generic helper.

interface Point {
    readonly x: number;
    readonly y: number;
}

type Shape =
    | { kind: "circle"; center: Point; radius: number }
    | { kind: "rect"; min: Point; max: Point };

/* The switch is exhaustive: a new kind of shape fails to
   compile until it is handled here. */
function area(shape: Shape): number {
    switch (shape.kind) {
        case "circle":
            return Math.PI * shape.radius ** 2;
        case "rect": {
            const w = shape.max.x - shape.min.x;
            const h = shape.max.y - shape.min.y;
            return Math.abs(w * h);
        }
    }
}

export function largest<T>(
    items: readonly T[],
    score: (item: T) => number,
): T | undefined {
    let best: T | undefined = undefined;
    for (const item of items) {
        if (best === undefined || score(item) > score(best)) best = item;
    }
    return best;
}

const shapes: Shape[] = [
    { kind: "circle", center: { x: 0, y: 0 }, radius: 1.5 },
    { kind: "rect", min: { x: 0, y: 0 }, max: { x: 0x10, y: 2 } },
];
const big = largest(shapes, area);
console.log(`largest: ${big?.kind ?? "none"}`, big !== null);
