module M : sig
  module F : sig type u end
  type z = Zed
end = struct
  module F = struct type u = int end
  type z = Zed
end
type w = Wed
