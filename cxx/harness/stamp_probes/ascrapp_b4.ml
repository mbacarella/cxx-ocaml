module P = Set.Make(String)
module Q = Hashtbl.Make(String)
module M : sig end = struct module S = Hashtbl.Make(Char) end
