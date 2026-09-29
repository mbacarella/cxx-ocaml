module M : sig type u end = struct
  module P = struct module S = Set.Make (String) end
  type v = P.S.t
  type u = int
end
