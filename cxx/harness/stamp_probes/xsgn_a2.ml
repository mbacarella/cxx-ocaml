(* `as` has the lowest pattern precedence: it aliases everything parsed so
   far, and `::`, `,` and `|` continue after it (dune's
   `(`Geq|`Gt|`Neq as op, i) :: r`) *)
let a = function (`Geq|`Gt|`Neq as op, i) :: _ -> Some (op, i) | _ -> None
let b = function 1 | 2 as x, y -> x + y | _ -> 0
let c = function a, b as x, c -> fst x + a + b + c
let d = function (a as x) :: _ as l, (2 as y) -> x + y + List.length l + a | _ -> 0
let e = function a :: b as x :: _ -> List.length x + a + List.length b | _ -> 0
let f = function Some a as x, b | (None as x), (b as a) -> ignore x; a + b
let g = function (a as x [@foo]) :: _ -> a + x | [] -> 0
let h = function a, b as x :: _ -> fst x + a + b | [] -> 0
