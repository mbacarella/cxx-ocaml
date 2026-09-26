let f = function Some x -> x | None -> 0
let g x = function 0 -> x | _ -> 0
let h = fun x -> function [] -> x | y :: _ -> y
