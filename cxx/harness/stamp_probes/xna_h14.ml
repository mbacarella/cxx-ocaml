module M : sig module XSet : sig type t end end = struct module XSet = Set.Make(
  String ) type u = XSet.t end
