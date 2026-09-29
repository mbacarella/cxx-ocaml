module Create(P: sig type t end) = struct
    class virtual ['archiver] agent =
        object(self:'self)
            method private put:
                type a b. 'archiver -> (int -> b) ->
                (a, unit, string, string, string, b) format6 -> a
                = fun level cont ->
                    let f message =
                        cont 1
                    in
                    Printf.ksprintf f
        end
end
