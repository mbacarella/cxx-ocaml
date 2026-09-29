module M : sig module S : Set.S with type elt = string end =
  struct module S = Set.Make(String) end
