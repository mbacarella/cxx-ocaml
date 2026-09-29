let g f = List.map (f : ?x:int -> int -> unit) [1]
