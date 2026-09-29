module M : sig module XSet : sig type t end end = struct module XSet = Set.Make(
  Bool ) end type u = Set.Make( String ).t
