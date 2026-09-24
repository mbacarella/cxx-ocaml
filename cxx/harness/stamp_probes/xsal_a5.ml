(* control: a local module shadows the stdlib member *)
module List = struct let x = 1 end
module M = List
