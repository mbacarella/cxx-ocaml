module M = struct type t = int let compare = compare end
module N : sig end =
  struct module S = Set.Make (String) module T = Set.Make (M) end
