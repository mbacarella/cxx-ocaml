module M : sig module N : sig end end =
  struct module N = struct module S = Set.Make(String) end end
