type z = int
module M = struct module S = Set.Make (Int) end
let f () = (module M.S : Set.S with type elt = int)
