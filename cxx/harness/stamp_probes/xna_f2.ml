module M : sig module XSet : sig type t end end = struct module XSet = Set.Make(
  String ) end module N = Set.Make( Bool )
