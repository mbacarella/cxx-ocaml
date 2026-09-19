module M : sig module XSet : sig type t end end = struct module XSet = Set.Make(
  String ) end module N = struct module XSet = Set.Make( Bool ) end
