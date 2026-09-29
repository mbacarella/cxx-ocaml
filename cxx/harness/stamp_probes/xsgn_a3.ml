(* an open-ended expression (let, match, fun, try) whose body ends with a
   trailing `;` is the left operand of a following infix operator; a
   labeled tuple ending in a labeled element too; `e; ~l:x, y` is a
   sequence *)
let f x = x
let a = let x = 1 in ignore x; |> f
let b = match 1 with _ -> (); |> f
let c = (fun x -> x; |> f) 1
let d = try (); with _ -> (); |> f
let e = let x = 1 in x; , 2
let g = let x = [1] in x; @ [2]
let h = ~a:1, ~b:2 |> f
let i = ~a:1, 2 |> f
let j = let x = 1 in ignore x; ~l:1, 2
let k = let ~p, ~q = ~p:1, ~q:2 |> f in p + q
