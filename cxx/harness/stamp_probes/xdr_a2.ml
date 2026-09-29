module M : sig type u end =
  struct module S = Set.Make (String) type u = S.elt end
