module M : sig module XSet : sig type t end end = struct module XSet = Set.Make(
  String ) end module N : sig end = struct module XSet = Set.Make( String ) end
