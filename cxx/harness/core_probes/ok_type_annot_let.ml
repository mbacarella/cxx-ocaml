let x : int list = [1]
let f : ?x:int -> unit -> int = fun ?(x = 0) () -> x
let (a : int), (b : string) = 1, "x"
