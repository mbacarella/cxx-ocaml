type t = Circle of float | Rect of float * float
let area = function Circle r -> 3.14159 *. r *. r | Rect (w, h) -> w *. h
let describe s = Printf.sprintf "%s with area %.2f" (match s with Circle _ -> "circle" | Rect _ -> "rect") (area s)
