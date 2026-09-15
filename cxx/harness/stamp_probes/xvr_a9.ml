module M : sig type u end = struct
  module S = Map.Make (String)
  type u = int
  let y = fun () -> S.empty
end
