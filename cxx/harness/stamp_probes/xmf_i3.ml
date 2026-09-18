module M = struct
  include Sys.Immediate64.Make(Int)(Int64)
  module type S = sig val zero : t val one : t val add : t -> t -> t end
end
