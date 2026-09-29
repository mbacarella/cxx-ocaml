module M : sig
  open Set.Make(Bool)
  type u = t
end = struct
  type u = Set.Make(Bool).t
end
