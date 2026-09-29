let f1 ?x y = ignore x; ignore y
let c : ?random:bool -> int -> (int, int) Hashtbl.t = Hashtbl.create
let h = List.map c [1]
