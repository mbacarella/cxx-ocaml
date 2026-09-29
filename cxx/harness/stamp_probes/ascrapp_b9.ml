module P = Set.Make(String)
module Y = struct type t = int let compare = compare end
module M : sig end = struct module S = Set.Make(Y) end
