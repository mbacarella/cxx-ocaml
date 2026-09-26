let f (type a) (l : a list) = List.length l
let g : type a. a -> a = fun x -> x
let rec h : 'a. 'a list -> int = function [] -> 0 | _ :: t -> 1 + h t
