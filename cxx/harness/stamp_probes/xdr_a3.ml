module M : sig type u end =
  struct module S = Set.Make (String) type v = S.t option type u = int end
