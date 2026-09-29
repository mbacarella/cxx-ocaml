module M = struct type t = int let equal = (=) let hash = Hashtbl.hash end
module N : sig end =
  struct module P = struct module S = Hashtbl.Make (M) end end
