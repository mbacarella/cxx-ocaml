module M : sig type u end = struct
  module S = Set.Make (String)
  external y : S.t -> int = "f"
  type u = int
end
