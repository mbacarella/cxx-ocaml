module M : sig end = struct module XSet = Set.Make( String ) end module N =
  Set.Make( Bool )
