module P = Set.Make(String)
module M : sig end =
  struct module S = Set.Make(Char) module T = Set.Make(Char) end
