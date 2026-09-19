module M : sig module XSet : sig type t end end = struct module XSet = Set.Make(
  String ) end
  type u = Set.Make( String ).t
