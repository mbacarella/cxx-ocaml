module M : sig module XSet : sig type t end end = struct module XSet = Set.Make(
  String ) let _ = XSet.empty end
