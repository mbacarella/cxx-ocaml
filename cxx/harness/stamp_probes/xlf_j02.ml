module M : sig end = struct module N = struct module S =
  Set.Make(String) end end
