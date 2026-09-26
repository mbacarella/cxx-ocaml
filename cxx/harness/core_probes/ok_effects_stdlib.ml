let f () = Effect.perform (Effect.Deep.Continuation_already_resumed |> fun _ -> assert false)
