let rec depth : 'a. 'a list -> int = fun l -> match l with [] -> 0 | _ :: t -> 1 + depth t
let id : 'a. 'a -> 'a = fun x -> x
