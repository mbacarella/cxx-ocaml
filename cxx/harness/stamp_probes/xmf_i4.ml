module M = struct
  include Sys.Immediate64.Make(Int)(Int64)
  module type S = sig val zero : t val one : t val add : t -> t -> t end
  let impl : (module S) =
    match repr with
    | Immediate -> (module Int : S)
    | Non_immediate -> (module Int64 : S)
end
