module M : sig type u end =
  struct module S = Set.Make (String) type v = { f : S.elt } type u = int end
