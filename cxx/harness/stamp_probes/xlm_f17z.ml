module F (X : sig end) = struct
  let f () =
    let module N = Map.Make(String) in
    ()
end
