module M : sig module XSet : sig type t end type u = XSet.t end = struct module
  XSet = Set.Make( String ) type u = XSet.t end
