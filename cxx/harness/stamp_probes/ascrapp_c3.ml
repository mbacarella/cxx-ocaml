module M : sig module S : sig type elt = string type t val empty : t end end =
  struct module S = Set.Make(String) end
