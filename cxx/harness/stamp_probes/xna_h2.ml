module M : sig module N : sig type t end end = struct module N = struct module
  XSet = Set.Make( String ) type t = XSet.t end end
