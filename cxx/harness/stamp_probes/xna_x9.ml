module M : sig module A : sig type t end end = struct module A = Set.Make(
  String ) end module B = Set.Make( Bool )
