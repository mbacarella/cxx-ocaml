module M : sig type u end = struct
  module S = Map.Make (String)
  type u = int
  let y = try S.cardinal with _ -> S.cardinal
end
